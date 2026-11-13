#include <pebble.h>

#define BOARD_SIZE 20
#define CACHE_SIZE 32
#define HEADER_COLOR GColorOxfordBlue
#define SELECTED_COLOR GColorDukeBlue

typedef struct {
  int index;
  bool loaded;
  char name[200];
  char detail[224];
} StopRow;

typedef struct {
  time_t arrival;
  bool live;
  uint8_t vehicle;
  char route[24];
  char next[132];
} ArrivalRow;

typedef struct {
  int index;
  time_t expires;
  ArrivalRow rows[2];
} Preview;

static Window *s_window;
static MenuLayer *s_menu;
static GFont s_font, s_small_font;
static AppTimer *s_timeout, *s_auto_timer, *s_row_timer;
static AppTimer *s_marquee_timer;
static int s_marquee_row = -1;
static int s_marquee_offset;
static StopRow s_stops[CACHE_SIZE];
static ArrivalRow s_board[BOARD_SIZE], s_next_board[BOARD_SIZE];
static unsigned s_request, s_generation, s_board_request;
static Preview s_previews[CACHE_SIZE];
static bool s_intermediate, s_final_ready, s_waiting_final;
static int s_focus_sent = -2;
static AppTimer *s_focus_timer;
static int s_stop_count, s_selected, s_count, s_next_count, s_next_index;
static int s_requested_row = -1;
static time_t s_expiry, s_next_expiry;
static bool s_ready, s_on_board, s_busy, s_partial;
static const char *s_notice = "Подключение к телефону...";
static char s_title[200];

static void refresh_board(bool automatic);

static void redraw(void) {
  menu_layer_reload_data(s_menu);
}

static void cancel_timer(AppTimer **timer) {
  if (*timer) {
    app_timer_cancel(*timer);
    *timer = NULL;
  }
}

static void failed(const char *notice) {
  cancel_timer(&s_timeout);
  s_busy = false;
  s_waiting_final = false;
  s_requested_row = -1;
  s_request++;
  s_notice = s_on_board && s_count ? "Не удалось обновить" : notice;
  redraw();
}

static void timed_out(void *context) {
  s_timeout = NULL;
  failed("Проверьте связь с телефоном");
}

static bool send(int command, int index) {
  DictionaryIterator *outbox;
  unsigned request = command == 6 ? s_request : ++s_request;
  if (app_message_outbox_begin(&outbox) != APP_MSG_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Command, command) != DICT_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Request, request) != DICT_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Index, index) != DICT_OK ||
      app_message_outbox_send() != APP_MSG_OK) {
    if (command != 6) failed("Проверьте связь с телефоном");
    return false;
  }
  if (command == 6) {
    return true;
  }
  if (command == 3 || command == 4 || command == 8) {
    s_board_request = s_request;
    s_waiting_final = false;
    s_final_ready = false;
  }
  cancel_timer(&s_timeout);
  s_timeout = app_timer_register(command == 3 || command == 4 || command == 8 ? 90000 : 15000,
                                 timed_out, NULL);
  return true;
}

static void search(void) {
  s_stop_count = 0;
  memset(s_stops, 0, sizeof(s_stops));
  memset(s_previews, 0, sizeof(s_previews));
  s_generation = 0;
  s_focus_sent = -2;
  s_requested_row = -1;
  s_notice = s_ready ? "Поиск остановок..." : "Подключение к телефону...";
  s_busy = s_ready;
  if (s_ready) send(1, 0);
  redraw();
}

static void auto_refresh(void *context) {
  s_auto_timer = NULL;
  if (!s_on_board) return;
  refresh_board(true);
  s_auto_timer = app_timer_register(60000, auto_refresh, NULL);
}

static void refresh_board(bool automatic) {
  if (s_busy) return;
  if (!s_count) s_notice = "Загрузка табло...";
  s_busy = true;
  send(automatic ? 4 : 3, s_selected);
  redraw();
}

static int number(DictionaryIterator *iterator, uint32_t key) {
  Tuple *tuple = dict_find(iterator, key);
  if (!tuple || tuple->length != sizeof(int32_t) ||
      (tuple->type != TUPLE_INT && tuple->type != TUPLE_UINT)) return 0;
  return tuple->value->int32;
}

static void copy_text(DictionaryIterator *iterator, uint32_t key, char *out, size_t size) {
  Tuple *tuple = dict_find(iterator, key);
  if (!tuple || tuple->type != TUPLE_CSTRING) {
    out[0] = 0;
    return;
  }
  const char *text = (const char *)tuple->value;
  size_t length = 0;
  while (length < tuple->length && text[length]) length++;
  if (length >= size) {
    length = size - 1;
    while (length && (text[length] & 0xc0) == 0x80) length--;
  }
  memcpy(out, text, length);
  out[length] = 0;
}

static void notify_focus(void *context) {
  s_focus_timer = NULL;
  if (s_on_board) return;
  if (s_timeout) {
    s_focus_timer = app_timer_register(150, notify_focus, NULL);
    return;
  }
  int index = menu_layer_get_selected_index(s_menu).row;
  if (index >= s_stop_count) index = -1;
  if (index != s_focus_sent && s_ready) {
    if (send(6, index)) s_focus_sent = index;
    else s_focus_timer = app_timer_register(150, notify_focus, NULL);
  }
}

static void update_focus(void) {
  cancel_timer(&s_focus_timer);
  s_focus_timer = app_timer_register(100, notify_focus, NULL);
}

static void commit_board(void) {
  memcpy(s_board, s_next_board, s_next_count * sizeof(s_board[0]));
  s_count = s_next_count;
  s_expiry = s_next_expiry;
  s_busy = s_intermediate;
  s_waiting_final = s_intermediate;
  if (s_intermediate) {
    if (s_final_ready) {
      s_waiting_final = false;
      send(7, 0);
    }
    else s_timeout = app_timer_register(90000, timed_out, NULL);
  }
  s_notice = s_partial ? "Расписание загружено частично" : NULL;
  redraw();
}

static void inbox(DictionaryIterator *iterator, void *context) {
  if (dict_find(iterator, MESSAGE_KEY_PhoneReady)) {
    s_ready = true;
    if (s_on_board) {
      s_busy = false;
      refresh_board(false);
    } else {
      search();
    }
    return;
  }
  int status = number(iterator, MESSAGE_KEY_Status);
  int index = number(iterator, MESSAGE_KEY_Index);
  if (status == 6) {
    if (!s_generation || (unsigned)number(iterator, MESSAGE_KEY_Generation) != s_generation ||
        index < 0 || index >= s_stop_count) return;
    Preview *preview = &s_previews[index % CACHE_SIZE];
    preview->index = index;
    preview->expires = number(iterator, MESSAGE_KEY_Expires);
    preview->rows[0].arrival = number(iterator, MESSAGE_KEY_Arrival);
    preview->rows[0].live = number(iterator, MESSAGE_KEY_Live);
    preview->rows[0].vehicle = number(iterator, MESSAGE_KEY_Vehicle);
    copy_text(iterator, MESSAGE_KEY_NextStop, preview->rows[0].next, sizeof(preview->rows[0].next));
    copy_text(iterator, MESSAGE_KEY_Route, preview->rows[0].route, sizeof(preview->rows[0].route));
    preview->rows[1].arrival = number(iterator, MESSAGE_KEY_SecondArrival);
    preview->rows[1].live = number(iterator, MESSAGE_KEY_SecondLive);
    preview->rows[1].vehicle = number(iterator, MESSAGE_KEY_SecondVehicle);
    copy_text(iterator, MESSAGE_KEY_SecondNextStop, preview->rows[1].next, sizeof(preview->rows[1].next));
    copy_text(iterator, MESSAGE_KEY_SecondRoute, preview->rows[1].route, sizeof(preview->rows[1].route));
    if (!s_on_board) redraw();
    return;
  }
  if (status == 8) {
    if (!s_on_board || !s_busy ||
        (unsigned)number(iterator, MESSAGE_KEY_Generation) != s_board_request) return;
    s_final_ready = true;
    if (s_waiting_final) {
      s_waiting_final = false;
      send(7, 0);
    }
    return;
  }
  if (!s_timeout || !dict_find(iterator, MESSAGE_KEY_Request) ||
      (unsigned)number(iterator, MESSAGE_KEY_Request) != s_request) return;
  cancel_timer(&s_timeout);
  if (status < 0) {
    failed(status == -1 ? "Проверьте геолокацию телефона" : "Данные недоступны");
  } else if (status == 1) {
    s_busy = false;
    s_generation = s_request;
    s_stop_count = number(iterator, MESSAGE_KEY_Total);
    update_focus();
    s_notice = s_stop_count ? NULL : "Рядом нет остановок";
    redraw();
  } else if (status == 2 && !s_on_board && index == s_requested_row) {
    StopRow *row = &s_stops[index % CACHE_SIZE];
    row->index = index;
    row->loaded = true;
    copy_text(iterator, MESSAGE_KEY_Name, row->name, sizeof(row->name));
    copy_text(iterator, MESSAGE_KEY_Detail, row->detail, sizeof(row->detail));
    s_requested_row = -1;
    redraw();
  } else if ((status == 3 || status == 4 || status == 7) && s_on_board) {
    s_next_count = number(iterator, MESSAGE_KEY_Total);
    if (s_next_count < 0 || s_next_count > BOARD_SIZE) {
      failed("Данные недоступны");
      return;
    }
    s_next_expiry = number(iterator, MESSAGE_KEY_Expires);
    s_next_index = 0;
    s_partial = status == 4;
    s_intermediate = status == 7;
    s_waiting_final = false;
    if (s_next_count) send(5, 0);
    else commit_board();
  } else if (status == 5 && s_on_board && index == s_next_index && index < s_next_count) {
    ArrivalRow *row = &s_next_board[index];
    row->arrival = number(iterator, MESSAGE_KEY_Arrival);
    row->live = number(iterator, MESSAGE_KEY_Live);
    row->vehicle = number(iterator, MESSAGE_KEY_Vehicle);
    copy_text(iterator, MESSAGE_KEY_NextStop, row->next, sizeof(row->next));
    copy_text(iterator, MESSAGE_KEY_Route, row->route, sizeof(row->route));
    if (++s_next_index < s_next_count) send(5, s_next_index);
    else commit_board();
  }
}

static void request_row(void *context) {
  s_row_timer = NULL;
  if (!s_on_board && s_requested_row >= 0) send(2, s_requested_row);
}

static void outbox_failed(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  if (number(iterator, MESSAGE_KEY_Command) == 6) {
    s_focus_sent = -2;
    update_focus();
  } else {
    failed("Проверьте связь с телефоном");
  }
}

typedef struct {
  const ArrivalRow *first;
  int times[2][3];
  int counts[2];
} RouteGroup;

static int route_groups(RouteGroup groups[BOARD_SIZE]) {
  int count = 0;
  time_t now = time(NULL);
  for (int i = 0; i < s_count; i++) {
    const ArrivalRow *row = &s_board[i];
    if (row->arrival <= now || (row->live && s_expiry <= now)) continue;

    int group = 0;
    while (group < count) {
      const ArrivalRow *first = groups[group].first;
      if (first->vehicle == row->vehicle && strcmp(first->route, row->route) == 0 &&
          strcmp(first->next, row->next) == 0) break;
      group++;
    }
    if (group == count) {
      groups[count++] = (RouteGroup){ .first = row };
    }

    RouteGroup *entry = &groups[group];
    int source = row->live ? 0 : 1;
    int slot = entry->counts[source];
    if (slot < 3) {
      entry->times[source][slot] = (int)difftime(row->arrival, now);
      entry->counts[source]++;
    }
  }
  return count;
}

static uint16_t row_count(MenuLayer *menu, uint16_t section, void *context) {
  if (!s_on_board) return s_stop_count + 1;
  static RouteGroup groups[BOARD_SIZE];
  int count = route_groups(groups);
  return count ? count : 1;
}

static int16_t row_height(MenuLayer *menu, MenuIndex *index, void *context) {
  if (!s_on_board) {
    if (index->row >= s_stop_count) return 57;
    Preview *preview = &s_previews[index->row % CACHE_SIZE];
    if (preview->index != index->row) return 128;
    return 128 + 18 * (!!preview->rows[0].next[0] + !!preview->rows[1].next[0]);
  }
  static RouteGroup groups[BOARD_SIZE];
  int count = route_groups(groups);
  if (index->row >= count) return 48;
  RouteGroup *group = &groups[index->row];
  return (group->counts[0] && group->counts[1] ? 68 : 48) +
         (group->first->next[0] ? 18 : 0);
}

// Keep rendering helpers out of the menu callback's stack frame.
static __attribute__((noinline)) void draw_text(GContext *ctx, const char *text,
                                                 GRect bounds, bool selected) {
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, text, s_font, bounds, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

static void reset_marquee(void) {
  cancel_timer(&s_marquee_timer);
  s_marquee_row = -1;
  s_marquee_offset = 0;
}

static void marquee_tick(void *context) {
  s_marquee_timer = NULL;
  if ((intptr_t)context) s_marquee_offset = 0;
  else s_marquee_offset += 2;
  layer_mark_dirty(menu_layer_get_layer(s_menu));
}

static __attribute__((noinline)) void draw_direction(GContext *ctx, const char *text, GRect line,
                                                      int row, bool selected) {
  GRect measure = GRect(0, 0, 2048, line.size.h);
  int width = graphics_text_layout_get_content_size(text, s_font, measure,
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).w;
  if (!selected || width <= line.size.w) {
    if (selected) reset_marquee();
    draw_text(ctx, text, line, selected);
    return;
  }

  if (s_marquee_row != row) {
    reset_marquee();
    s_marquee_row = row;
  }
  int limit = width - line.size.w;
  if (s_marquee_offset > limit) s_marquee_offset = limit;
  GRect moving = GRect(line.origin.x - s_marquee_offset, line.origin.y,
                       width + 4, line.size.h);
  draw_text(ctx, text, moving, true);
  graphics_context_set_fill_color(ctx, SELECTED_COLOR);
  graphics_fill_rect(ctx, GRect(0, line.origin.y, line.origin.x, line.size.h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(line.origin.x + line.size.w, line.origin.y,
                                line.origin.x, line.size.h), 0, GCornerNone);

  if (!s_marquee_timer) {
    int delay = s_marquee_offset == 0 || s_marquee_offset == limit ? 900 : 65;
    s_marquee_timer = app_timer_register(delay, marquee_tick, (void *)(intptr_t)(s_marquee_offset == limit));
  }
}

static __attribute__((noinline)) void draw_vehicle(GContext *ctx, GPoint position, uint8_t vehicle) {
  if (!vehicle) return;
  int x = position.x, y = position.y;
  GColor color = vehicle == 3 ? GColorWindsorTan :
                 vehicle == 2 ? GColorDarkGreen : GColorOxfordBlue;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(x, y, 24, 20), 4, GCornersAll);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  if (vehicle == 3) {
    graphics_draw_line(ctx, GPoint(x + 9, y + 5), GPoint(x + 14, y + 2));
    graphics_draw_line(ctx, GPoint(x + 7, y + 17), GPoint(x + 9, y + 14));
    graphics_draw_line(ctx, GPoint(x + 16, y + 17), GPoint(x + 14, y + 14));
    graphics_fill_rect(ctx, GRect(x + 7, y + 6, 10, 9), 2, GCornersAll);
  } else {
    graphics_fill_rect(ctx, GRect(x + 5, y + 5, 14, 10), 2, GCornersAll);
    graphics_fill_circle(ctx, GPoint(x + 8, y + 15), 2);
    graphics_fill_circle(ctx, GPoint(x + 16, y + 15), 2);
  }
  graphics_context_set_fill_color(ctx, color);
  if (vehicle == 2) {
    graphics_context_set_stroke_color(ctx, color);
    graphics_draw_line(ctx, GPoint(x + 13, y + 6), GPoint(x + 10, y + 10));
    graphics_draw_line(ctx, GPoint(x + 10, y + 10), GPoint(x + 13, y + 10));
    graphics_draw_line(ctx, GPoint(x + 13, y + 10), GPoint(x + 11, y + 14));
  } else {
    graphics_fill_rect(ctx, GRect(x + 8, y + 8, 8, 3), 0, GCornerNone);
  }
}

static __attribute__((noinline)) void draw_arrival(GContext *ctx, const ArrivalRow *row, GRect line,
                                                    bool selected) {
  int seconds = (int)difftime(row->arrival, time(NULL));
  char eta[24], text[64];
  if (seconds < 60) snprintf(eta, sizeof(eta), "< 1 мин");
  else snprintf(eta, sizeof(eta), "%d мин", (seconds + 59) / 60);
  snprintf(text, sizeof(text), "%s · %s", row->route, eta);
  if (row->vehicle) {
    draw_vehicle(ctx, line.origin, row->vehicle);
    line.origin.x += 29;
    line.size.w -= 29;
  }
  const char *source = row->live ? "прогноз" : "по расп.";
  int width = graphics_text_layout_get_content_size(source, s_small_font,
    GRect(0, 0, 200, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).w + 2;
  GRect label = GRect(line.origin.x + line.size.w - width, line.origin.y, width, 22);
  line.size.w -= width + 4;
  line.origin.y += 2;
  label.origin.y += 2;
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, text, s_small_font, line, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, source, s_small_font, label, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentRight, NULL);
}

static void draw_route_group(GContext *ctx, const RouteGroup *group, int width, bool selected) {
  const ArrivalRow *first = group->first;
  draw_vehicle(ctx, GPoint(8, 3), first->vehicle);
  int title_x = first->vehicle ? 37 : 8;
  draw_text(ctx, first->route, GRect(title_x, 2, width - title_x - 8, 22), selected);

  int y = 24;
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  if (first->next[0]) {
    graphics_draw_text(ctx, first->next, s_small_font, GRect(title_x, y, width - title_x - 8, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y += 18;
  }
  for (int source = 0; source < 2; source++) {
    if (!group->counts[source]) continue;
    char times[48] = "";
    for (int i = 0; i < group->counts[source]; i++) {
      int seconds = group->times[source][i];
      char value[12];
      if (seconds < 60) snprintf(value, sizeof(value), "<1");
      else snprintf(value, sizeof(value), "%d", (seconds + 59) / 60);
      size_t used = strlen(times);
      snprintf(times + used, sizeof(times) - used, "%s%s", i ? ", " : "", value);
    }
    size_t used = strlen(times);
    snprintf(times + used, sizeof(times) - used, " мин");
    graphics_draw_text(ctx, source ? "по расп." : "прогноз", s_small_font,
                       GRect(8, y, 62, 20), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, times, s_small_font, GRect(72, y, width - 80, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y += 20;
  }
}

static __attribute__((noinline)) void draw_board_row(GContext *ctx, uint16_t row,
                                                        int width, bool selected) {
  GRect line = GRect(8, 4, width - 16, 48);
  static RouteGroup groups[BOARD_SIZE];
  if (row >= route_groups(groups)) {
    draw_text(ctx, s_notice ? s_notice : "Нет данных о прибытии", line, selected);
    return;
  }
  draw_route_group(ctx, &groups[row], width, selected);
}

static void draw_row(GContext *ctx, const Layer *layer, MenuIndex *index, void *context) {
  GRect bounds = layer_get_bounds(layer);
  bool selected = menu_cell_layer_is_highlighted(layer);
  GRect line = GRect(8, 4, bounds.size.w - 16, 48);
  if (s_on_board) {
    draw_board_row(ctx, index->row, bounds.size.w, selected);
    return;
  }

  if (index->row == s_stop_count) {
    draw_text(ctx, s_notice ? s_notice : "Обновить", line, selected);
    return;
  }
  StopRow *row = &s_stops[index->row % CACHE_SIZE];
  if (!row->loaded || row->index != index->row) {
    if (s_requested_row < 0 && !s_busy && !s_notice) {
      s_requested_row = index->row;
      s_row_timer = app_timer_register(1, request_row, NULL);
    }
    draw_text(ctx, "Загрузка...", line, selected);
    return;
  }
  line.size.h = 42;
  draw_text(ctx, row->name, line, selected);
  line.origin.y = 43;
  line.size.h = 22;
  const char *direction = strchr(row->detail, '\n');
  char distance[24];
  size_t length = direction ? (size_t)(direction - row->detail) : strlen(row->detail);
  if (length >= sizeof(distance)) length = sizeof(distance) - 1;
  memcpy(distance, row->detail, length);
  distance[length] = '\0';
  draw_text(ctx, distance, line, selected);
  if (direction) {
    line.origin.y += 20;
    draw_direction(ctx, direction + 1, line, index->row, selected);
  }
  Preview *preview = &s_previews[index->row % CACHE_SIZE];
  if (preview->index != index->row) return;
  time_t now = time(NULL);
  int y = 84;
  for (int i = 0; i < 2; i++) {
    ArrivalRow *arrival = &preview->rows[i];
    int seconds = (int)difftime(arrival->arrival, now);
    if (seconds <= 0 || (arrival->live && preview->expires <= now)) continue;
    draw_arrival(ctx, arrival, GRect(8, y, bounds.size.w - 16, 22), selected);
    y += 20;
    if (arrival->next[0]) {
      graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
      graphics_draw_text(ctx, arrival->next, s_small_font,
                         GRect(37, y, bounds.size.w - 45, 18),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      y += 18;
    }
  }
}

static int16_t header_height(MenuLayer *menu, uint16_t section, void *context) {
  return s_on_board && s_notice && s_count ? 90 : 48;
}

static void draw_header(GContext *ctx, const Layer *layer, uint16_t section, void *context) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, HEADER_COLOR);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  draw_text(ctx, s_on_board ? s_title : "Ближайшие остановки",
            GRect(8, 3, bounds.size.w - 16, 43), true);
  if (s_on_board && s_notice && s_count) {
    draw_text(ctx, s_notice, GRect(8, 46, bounds.size.w - 16, 42), true);
  }
}

static void select_row(MenuLayer *menu, MenuIndex *index, void *context) {
  reset_marquee();
  if (s_on_board) {
    refresh_board(false);
  } else if (index->row == s_stop_count) {
    cancel_timer(&s_row_timer);
    search();
  } else {
    StopRow *row = &s_stops[index->row % CACHE_SIZE];
    if (!row->loaded || row->index != index->row) return;
    cancel_timer(&s_row_timer);
    cancel_timer(&s_timeout);
    s_requested_row = -1;
    s_selected = index->row;
    snprintf(s_title, sizeof(s_title), "%s", row->name);
    s_on_board = true;
    s_count = 0;
    Preview *preview = &s_previews[s_selected % CACHE_SIZE];
    if (preview->index == s_selected) {
      time_t now = time(NULL);
      s_expiry = preview->expires;
      for (int i = 0; i < 2; i++) {
        ArrivalRow *row = &preview->rows[i];
        if (row->arrival > now && (!row->live || s_expiry > now)) {
          s_board[s_count++] = *row;
        }
      }
    }
    s_busy = true;
    s_notice = s_count ? NULL : "Загрузка табло...";
    send(8, s_selected);
    redraw();
    menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
    s_auto_timer = app_timer_register(60000, auto_refresh, NULL);
  }
}

static void back(ClickRecognizerRef recognizer, void *context) {
  if (!s_on_board) {
    window_stack_pop(true);
    return;
  }
  cancel_timer(&s_auto_timer);
  cancel_timer(&s_timeout);
  s_request++;
  s_on_board = false;
  s_busy = false;
  s_notice = NULL;
  redraw();
  menu_layer_set_selected_index(s_menu, MenuIndex(0, s_selected), MenuRowAlignCenter, false);
  s_focus_sent = -2;
  update_focus();
}

static void move_selection(int delta) {
  MenuIndex index = menu_layer_get_selected_index(s_menu);
  int next = (int)index.row + delta;
  if (next >= 0 && next < row_count(s_menu, 0, NULL)) {
    reset_marquee();
    menu_layer_set_selected_index(s_menu, MenuIndex(0, next), MenuRowAlignCenter, true);
    if (!s_on_board) update_focus();
  }
}

static void up(ClickRecognizerRef recognizer, void *context) {
  move_selection(-1);
}

static void down(ClickRecognizerRef recognizer, void *context) {
  move_selection(1);
}

static void select_button(ClickRecognizerRef recognizer, void *context) {
  MenuIndex index = menu_layer_get_selected_index(s_menu);
  select_row(s_menu, &index, NULL);
}

static void clicks(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_button);
  window_single_click_subscribe(BUTTON_ID_BACK, back);
}

static void tick(struct tm *local_time, TimeUnits units) {
  if (local_time->tm_sec % 5 == 0) {
    if (s_on_board) redraw();
    else layer_mark_dirty(menu_layer_get_layer(s_menu));
  }
}

static __attribute__((noinline)) void init(void) {
  s_window = window_create();
  Layer *root = window_get_root_layer(s_window);
  s_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_NOTO_18));
  s_small_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_NOTO_14));
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = row_count, .get_cell_height = row_height, .draw_row = draw_row,
    .get_header_height = header_height, .draw_header = draw_header, .select_click = select_row,
  });
  menu_layer_set_highlight_colors(s_menu, SELECTED_COLOR, GColorWhite);
  window_set_click_config_provider(s_window, clicks);
  layer_add_child(root, menu_layer_get_layer(s_menu));
  window_stack_push(s_window, true);
  app_message_register_inbox_received(inbox);
  app_message_register_outbox_failed(outbox_failed);
  if (app_message_open(512, 64) == APP_MSG_OK) {
    s_timeout = app_timer_register(10000, timed_out, NULL);
  } else {
    failed("Проверьте связь с телефоном");
  }
  tick_timer_service_subscribe(SECOND_UNIT, tick);
}

int main(void) {
  init();
  app_event_loop();
  cancel_timer(&s_focus_timer);
  reset_marquee();
  tick_timer_service_unsubscribe();
  cancel_timer(&s_auto_timer);
  cancel_timer(&s_row_timer);
  cancel_timer(&s_timeout);
  app_message_deregister_callbacks();
  menu_layer_destroy(s_menu);
  fonts_unload_custom_font(s_small_font);
  fonts_unload_custom_font(s_font);
  window_destroy(s_window);
}
