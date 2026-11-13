#include <pebble.h>

#define BOARD_SIZE 20
#define CACHE_SIZE 32

typedef struct {
  int index;
  bool loaded;
  char name[200];
  char detail[160];
} StopRow;

typedef struct {
  time_t arrival;
  bool live;
  char route[24];
} ArrivalRow;

static Window *s_window;
static MenuLayer *s_menu;
static GFont s_font;
static AppTimer *s_timeout, *s_auto_timer, *s_row_timer;
static AppTimer *s_marquee_timer;
static int s_marquee_row = -1;
static int s_marquee_offset;
static StopRow s_stops[CACHE_SIZE];
static ArrivalRow s_board[BOARD_SIZE], s_next_board[BOARD_SIZE];
static unsigned s_request;
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
  if (app_message_outbox_begin(&outbox) != APP_MSG_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Command, command) != DICT_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Request, ++s_request) != DICT_OK ||
      dict_write_int32(outbox, MESSAGE_KEY_Index, index) != DICT_OK ||
      app_message_outbox_send() != APP_MSG_OK) {
    failed("Проверьте связь с телефоном");
    return false;
  }
  cancel_timer(&s_timeout);
  s_timeout = app_timer_register(command == 3 || command == 4 ? 90000 : 15000,
                                 timed_out, NULL);
  return true;
}

static void search(void) {
  s_stop_count = 0;
  memset(s_stops, 0, sizeof(s_stops));
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

static void commit_board(void) {
  memcpy(s_board, s_next_board, s_next_count * sizeof(s_board[0]));
  s_count = s_next_count;
  s_expiry = s_next_expiry;
  s_busy = false;
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
  if (!s_timeout || !dict_find(iterator, MESSAGE_KEY_Request) ||
      (unsigned)number(iterator, MESSAGE_KEY_Request) != s_request) return;
  cancel_timer(&s_timeout);
  int status = number(iterator, MESSAGE_KEY_Status);
  int index = number(iterator, MESSAGE_KEY_Index);
  if (status < 0) {
    failed(status == -1 ? "Проверьте геолокацию телефона" : "Данные недоступны");
  } else if (status == 1) {
    s_busy = false;
    s_stop_count = number(iterator, MESSAGE_KEY_Total);
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
  } else if ((status == 3 || status == 4) && s_on_board) {
    s_next_count = number(iterator, MESSAGE_KEY_Total);
    if (s_next_count < 0 || s_next_count > BOARD_SIZE) {
      failed("Данные недоступны");
      return;
    }
    s_next_expiry = number(iterator, MESSAGE_KEY_Expires);
    s_next_index = 0;
    s_partial = status == 4;
    if (s_next_count) send(5, 0);
    else commit_board();
  } else if (status == 5 && s_on_board && index == s_next_index && index < s_next_count) {
    ArrivalRow *row = &s_next_board[index];
    row->arrival = number(iterator, MESSAGE_KEY_Arrival);
    row->live = number(iterator, MESSAGE_KEY_Live);
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
  failed("Проверьте связь с телефоном");
}

static ArrivalRow *visible_arrival(int index) {
  time_t now = time(NULL);
  for (int i = 0; i < s_count; i++) {
    ArrivalRow *row = &s_board[i];
    if (row->arrival <= now || (row->live && s_expiry <= now)) continue;
    if (index-- == 0) return row;
  }
  return NULL;
}

static uint16_t row_count(MenuLayer *menu, uint16_t section, void *context) {
  if (!s_on_board) return s_stop_count + 1;
  int count = 0;
  while (visible_arrival(count)) count++;
  return count ? count : 1;
}

static int16_t row_height(MenuLayer *menu, MenuIndex *index, void *context) {
  return s_on_board ? 43 : 87;
}

static void draw_text(GContext *ctx, const char *text, GRect bounds, bool selected) {
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

static void draw_direction(GContext *ctx, const char *text, GRect line,
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
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, line.origin.y, line.origin.x, line.size.h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(line.origin.x + line.size.w, line.origin.y,
                                line.origin.x, line.size.h), 0, GCornerNone);

  if (!s_marquee_timer) {
    int delay = s_marquee_offset == 0 || s_marquee_offset == limit ? 900 : 65;
    s_marquee_timer = app_timer_register(delay, marquee_tick, (void *)(intptr_t)(s_marquee_offset == limit));
  }
}

static void draw_row(GContext *ctx, const Layer *layer, MenuIndex *index, void *context) {
  GRect bounds = layer_get_bounds(layer);
  bool selected = menu_cell_layer_is_highlighted(layer);
  GRect line = GRect(8, 4, bounds.size.w - 16, 48);
  if (s_on_board) {
    ArrivalRow *row = visible_arrival(index->row);
    if (!row) {
      draw_text(ctx, s_notice ? s_notice : "Нет данных о прибытии", line, selected);
      return;
    }
    int seconds = (int)difftime(row->arrival, time(NULL));
    char text[96];
    char eta[24];
    if (seconds < 60) snprintf(eta, sizeof(eta), "< 1 мин");
    else snprintf(eta, sizeof(eta), "%d мин", (seconds + 59) / 60);
    snprintf(text, sizeof(text), "%s · %s", row->route, eta);
    draw_text(ctx, text, GRect(8, 2, bounds.size.w - 16, 21), selected);
    draw_text(ctx, row->live ? "прогноз" : "по расп.",
              GRect(8, 21, bounds.size.w - 16, 21), selected);
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
  snprintf(distance, sizeof(distance), "%.*s",
           direction ? (int)(direction - row->detail) : (int)strlen(row->detail), row->detail);
  draw_text(ctx, distance, line, selected);
  if (direction) {
    line.origin.y += 20;
    draw_direction(ctx, direction + 1, line, index->row, selected);
  }
}

static int16_t header_height(MenuLayer *menu, uint16_t section, void *context) {
  return s_on_board && s_notice && s_count ? 90 : 48;
}

static void draw_header(GContext *ctx, const Layer *layer, uint16_t section, void *context) {
  GRect bounds = layer_get_bounds(layer);
  draw_text(ctx, s_on_board ? s_title : "Ближайшие остановки",
            GRect(8, 3, bounds.size.w - 16, 43), false);
  if (s_on_board && s_notice && s_count) {
    draw_text(ctx, s_notice, GRect(8, 46, bounds.size.w - 16, 42), false);
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
    refresh_board(false);
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
}

static void move_selection(int delta) {
  MenuIndex index = menu_layer_get_selected_index(s_menu);
  int next = (int)index.row + delta;
  if (next >= 0 && next < row_count(s_menu, 0, NULL)) {
    reset_marquee();
    menu_layer_set_selected_index(s_menu, MenuIndex(0, next), MenuRowAlignCenter, true);
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
  if (s_on_board && local_time->tm_sec % 5 == 0) redraw();
}

int main(void) {
  s_window = window_create();
  Layer *root = window_get_root_layer(s_window);
  s_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_NOTO_18));
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = row_count, .get_cell_height = row_height, .draw_row = draw_row,
    .get_header_height = header_height, .draw_header = draw_header, .select_click = select_row,
  });
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
  app_event_loop();
  reset_marquee();
  tick_timer_service_unsubscribe();
  cancel_timer(&s_auto_timer);
  cancel_timer(&s_row_timer);
  cancel_timer(&s_timeout);
  app_message_deregister_callbacks();
  menu_layer_destroy(s_menu);
  fonts_unload_custom_font(s_font);
  window_destroy(s_window);
}
