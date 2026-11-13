"""Build the local stop catalog from the public Moscow timetable and route cards."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import html
import json
import math
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

PORTAL = 'https://transport.mos.ru'
API = 'https://api.moscowapp.mos.ru/v8.2'
ROW = re.compile(
    r'href="/transport/schedule/route/([^"]+)"[^>]*>\s*'
    r'<div class="ts-number">(.*?)</div>\s*<div class="ts-title">(.*?)</div>',
    re.S,
)
lock = threading.Lock()
next_request = 0
rate_limited = threading.Event()


def fetch(url, cache):
    global next_request
    path = cache / hashlib.sha256(url.encode()).hexdigest()
    if path.exists():
        return path.read_text()
    with lock:
        now = time.monotonic()
        delay = max(0, next_request - now)
        next_request = now + delay + 0.3
    time.sleep(delay)
    if rate_limited.is_set():
        return ''
    if url.startswith(PORTAL):
        result = subprocess.run(
            ['curl', '-sS', '--max-time', '15', '-H', 'X-Requested-With: XMLHttpRequest',
             '-w', '\n%{http_code}', url], check=True, capture_output=True, text=True)
        body, _, status = result.stdout.rpartition('\n')
        if status != '200':
            raise HTTPError(url, int(status), 'Portal request failed', {}, None)
        temporary = path.with_suffix('.' + str(threading.get_ident()) + '.tmp')
        temporary.write_text(body)
        temporary.replace(path)
        return body
    request = Request(url, headers={'User-Agent': 'WatchBus catalog builder',
                                    'X-Requested-With': 'XMLHttpRequest'})
    try:
        with urlopen(request, timeout=15) as response:
            body = response.read().decode()
    except HTTPError as error:
        if error.code == 429:
            rate_limited.set()
        raise
    temporary = path.with_suffix('.' + str(threading.get_ident()) + '.tmp')
    temporary.write_text(body)
    temporary.replace(path)
    return body


def label(value):
    return ' '.join(html.unescape(re.sub(r'<[^>]+>', ' ', value)).split())


def endpoints(value):
    return sorted(re.sub(r'[^\w]', '', part.casefold().replace('ё', 'е'))
                  for part in re.split(r'\s+[–—-]\s+', value))


def list_routes(cache):
    first = fetch(PORTAL + '/transport/schedule', cache)
    pages = int(re.search(r'data-count-pages="(\d+)"', first)[1])
    for page in range(1, pages + 1):
        document = first if page == 1 else fetch(
            PORTAL + '/ru/ajax/App/V2_ScheduleV2Controller/getRoutesList?' +
            urlencode({'mgt_schedule[page]': page, 'page': page}), cache)
        rows = ROW.findall(document)
        if not rows:
            raise ValueError(f'Empty route page {page}')
        for route_id, number, title in rows:
            if 'icon-trolleybus' not in number:
                yield route_id, label(number), label(title), (
                    'Трамвай' if 'icon-tramway' in number else 'Автобус')


def route_stops(route, cache):
    data = json.loads(fetch(API + '/route_v3/' + route['id'], cache))
    stops = {}
    for direction in data['directions']:
        for path in direction['routePaths']:
            sequence = path['stops']
            for index, stop in enumerate(sequence):
                if not (stop.get('id') and stop.get('lat') and stop.get('lon')):
                    continue
                entry = stops.setdefault(stop['id'], {
                    **stop, 'next_names': set(), 'next_paths': {},
                })
                if index + 1 < len(sequence):
                    next_name = sequence[index + 1].get('name')
                    if next_name:
                        entry['next_names'].add(next_name)
                        entry['next_paths'].setdefault(path['id'], set()).add(next_name)
    return stops


def resolve(row, cache):
    route_id, number, title, kind = row
    search_number = re.sub(r'^З-', '', number, flags=re.I)
    query = urlencode({'query': search_number, 'types': 'Route'})
    hits = json.loads(fetch(API + '/suggest?' + query, cache))['data']
    candidates = {item['id']: item for group in hits
                  for item in [group, *group.get('items', [])]
                  if item.get('name') == kind
                  and item.get('route', {}).get('number', '').casefold() == search_number.casefold()}
    exact = [item for item in candidates.values()
             if endpoints(item.get('description', '')) == endpoints(title)]
    if len(exact) == 1 and search_number == number:
        return route_stops(exact[0], cache)
    if not candidates:
        return {}
    document = fetch(PORTAL + '/transport/schedule/route/' + route_id, cache)
    points = [f['geometry']['coordinates']
              for raw in re.findall(r'data-coords="([^"]+)"', document)
              for f in json.loads(html.unescape(raw))['features']
              if f['geometry']['type'] == 'Point']
    matches = []
    for candidate in candidates.values():
        stops = route_stops(candidate, cache)
        if points and all(sum(math.hypot((s['lat']-y)*111000, (s['lon']-x)*62500) < 3
                              for s in stops.values()) == 1 for x, y in points):
            matches.append(stops)
    return matches[0] if len(matches) == 1 else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--limit', type=int)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    rows = list(dict.fromkeys(list_routes(args.cache)))
    rows = rows[:args.limit]
    catalog = {}
    missing = []
    directions = {}
    route_directions = {}

    def collect(row):
        if rate_limited.is_set():
            return {}
        try:
            return resolve(row, args.cache)
        except HTTPError as error:
            if error.code == 429:
                rate_limited.set()
            return {}
        except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
            print(f'{row[1]}: {type(error).__name__}', flush=True)
            return {}
    with ThreadPoolExecutor(max_workers=3) as pool:
        for index, (row, stops) in enumerate(zip(rows, pool.map(collect, rows)), 1):
            if not stops:
                missing.append(row[:3])
            for uuid, stop in stops.items():
                entry = catalog.setdefault(uuid, [uuid, stop['name'], stop['lat'], stop['lon'], []])
                directions.setdefault(uuid, set()).update(stop['next_names'])
                by_path, by_route = route_directions.setdefault(uuid, ({}, {}))
                for path_id, names in stop['next_paths'].items():
                    by_path.setdefault(path_id, set()).update(names)
                by_route.setdefault(row[1], set()).update(stop['next_names'])
                if [row[0], row[1]] not in entry[4]:
                    entry[4].append([row[0], row[1]])
            if index % 50 == 0:
                print(f'{index}/{len(rows)}: {len(catalog)} stops, {len(missing)} unresolved', flush=True)
    if rate_limited.is_set():
        raise SystemExit('Source returned HTTP 429. Rerun after the server pause.')
    for uuid, entry in catalog.items():
        known = sorted(directions[uuid])
        if known:
            extra = len(known) - 2
            suffix = f' (+{extra})' if extra > 0 else ''
            entry.append('Далее: ' + ' / '.join(known[:2]) + suffix)
    for uuid, (by_path, by_route) in route_directions.items():
        entry = catalog[uuid]
        if len(entry) == 5:
            entry.append(None)
        entry.append([{key: next(iter(names)) for key, names in source.items()
                       if len(names) == 1} for source in (by_path, by_route)])
    payload = {'routes': len(rows), 'unresolved': missing, 'stops': sorted(catalog.values())}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.tmp')
    temporary.write_text(json.dumps(payload, ensure_ascii=False, separators=(',', ':')) + '\n')
    temporary.replace(args.output)
    print(f'{len(rows)} routes, {len(catalog)} stops, {len(missing)} unresolved')


if __name__ == '__main__':
    main()
