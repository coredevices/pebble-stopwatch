#include <pebble.h>
#include "rendering.h"

#define PERSIST_DATA_KEY 100
#define PERSIST_DATA_VERSION 1 // the current version of the data. Increment this if changing format
#define PERSIST_DATA_VERSION_KEY 0

#define MSEC_IN_SEC 1000
#define MSEC_IN_MIN 60000
#define MSEC_IN_HR 3600000

#define DATA_MAX_LAP_COUNT 22
#define SCREEN_LAP_HEIGHT PBL_IF_ROUND_ELSE(14, 18)
#define SCREEN_LAP_FONT_SIZE 12
#define SCREEN_SCROLLING_OFF PBL_IF_ROUND_ELSE(86, 30)
#define SCREEN_SCROLL_STEP_SMALL 6
#define SCREEN_SCROLL_STEP_LARGE 18
#define SCREEN_BORDER_WIDTH PBL_IF_ROUND_ELSE(22, 4)
#define SCREEN_SPACING_WIDTH 4
#define SCREEN_MAX_LAPS_BEFORE_SCROLLING PBL_IF_ROUND_ELSE(5, 6)
#ifdef PBL_COLOR
#define HIGHLIGHT_COLOR GColorYellow
#else
#define HIGHLIGHT_COLOR GColorWhite
#endif

// lap identifiers
typedef enum {CurrentTime, CurrentLap, FirstLapHistory} Lap;

// window data structure, removes all global variables!
typedef struct {
  Layer           *drawing_layer;   //< layer for drawing everything on
  ActionBarLayer  *action_bar;      //< layer to receive clicks

  GFont           font_small;      //< modified LECO regular font
  GBitmap         *icon_down;       //< icon for scroll down button
  GBitmap         *icon_lap;        //< icon for lap button
  GBitmap         *icon_pause;      //< icon for pause button
  GBitmap         *icon_play;       //< icon for play button
  GBitmap         *icon_reset;      //< icon for reset button
  GBitmap         *icon_up;         //< icon for reset button

  int64_t         epochs_ms[DATA_MAX_LAP_COUNT + 2]; //< memory for timing, with first element
                                    //< being current time, and second current lap
  int32_t         lap_count;        //< total number of laps, excluding the current time
  int32_t         total_height_ani; //< animation value for total height of all items in window
  int32_t         total_height_off; //< offset of total height of all items in window

  AppTimer        *app_timer;       //< the refresh timer
} WindowData;


////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities
//

// get current epoch in milliseconds
static int64_t prv_get_epoch_ms(void) {
  time_t sec;
  uint16_t msec;
  time_ms(&sec, &msec);
  return (int64_t)sec * MSEC_IN_SEC + (int64_t)msec;
}

// format a millisecond duration into text
static void prv_format_duration_ms(char *buff_main, char *buff_ms, uint8_t size_main,
                                   uint8_t size_ms, int64_t duration, bool one_line) {
  // get time parts
  int hr = duration / MSEC_IN_HR;
  int min = (duration % MSEC_IN_HR) / MSEC_IN_MIN;
  int sec = (duration % MSEC_IN_MIN) / MSEC_IN_SEC;
  int csec = duration % MSEC_IN_SEC / 10;
  // format into string
  if (one_line) {
    snprintf(buff_main, size_main, "%d:%02d:%02d.%02d", hr, min, sec, csec);
    return;
  }
  if (hr > 0) {
    snprintf(buff_main, size_main, "%d:%02d", hr, min);
    snprintf(buff_ms, size_ms, "%02d", sec);
    return;
  }
  if (min > 0) {
    snprintf(buff_main, size_main, "%d:%02d", min, sec);
  }
  else {
    snprintf(buff_main, size_main, "%02d.", sec);
  }
  snprintf(buff_ms, size_ms, "%02d", csec);
}

// get if enough laps to scroll
static bool prv_get_scrolling_enabled(WindowData *data) {
  return (data->lap_count > SCREEN_MAX_LAPS_BEFORE_SCROLLING);
}

// get if scrolled to start
static bool prv_get_scrolling_reached_start(WindowData *data) {
  return prv_get_scrolling_enabled(data) && data->total_height_off >= 0;
}

// get if scrolled to end
static bool prv_get_scrolling_reached_end(WindowData *data) {
  return prv_get_scrolling_enabled(data) &&
    data->total_height_ani + SCREEN_SCROLLING_OFF < layer_get_bounds(data->drawing_layer).size.h;
}

// get scrolling start offset (pixels above top of scroll view)
static int32_t prv_get_scrolling_start_offset(WindowData *data) {
  return (int32_t)prv_get_scrolling_reached_start(data) * data->total_height_off;
}

// get scrolling end offset (pixels above bottom of screen)
static int32_t prv_get_scrolling_end_offset(WindowData *data) {
  return (int32_t)prv_get_scrolling_reached_end(data) *
    (layer_get_bounds(data->drawing_layer).size.h -
    (data->total_height_ani + SCREEN_SCROLLING_OFF));
}

// update all button icons
static void prv_update_icons(WindowData *data) {
  // check if paused
  if (data->epochs_ms[CurrentTime] > 0) {
    action_bar_layer_set_icon(data->action_bar, BUTTON_ID_SELECT, data->icon_pause);
    action_bar_layer_set_icon(data->action_bar, BUTTON_ID_UP, data->icon_lap);
    action_bar_layer_set_icon(data->action_bar, BUTTON_ID_DOWN, data->icon_reset);
#ifdef PBL_SDK_3
    action_bar_layer_set_icon_press_animation(data->action_bar, BUTTON_ID_UP,
                                              ActionBarLayerIconPressAnimationMoveLeft);
    action_bar_layer_set_icon_press_animation(data->action_bar, BUTTON_ID_DOWN,
                                              ActionBarLayerIconPressAnimationMoveLeft);
#endif
  }
  else {
    action_bar_layer_set_icon(data->action_bar, BUTTON_ID_SELECT, data->icon_play);
    if (prv_get_scrolling_enabled(data)) {
      action_bar_layer_set_icon(data->action_bar, BUTTON_ID_UP, data->icon_up);
      action_bar_layer_set_icon(data->action_bar, BUTTON_ID_DOWN, data->icon_down);
#ifdef PBL_SDK_3
      action_bar_layer_set_icon_press_animation(data->action_bar, BUTTON_ID_UP,
                                                ActionBarLayerIconPressAnimationMoveUp);
      action_bar_layer_set_icon_press_animation(data->action_bar, BUTTON_ID_DOWN,
                                                ActionBarLayerIconPressAnimationMoveDown);
#endif
    }
    else {
      action_bar_layer_clear_icon(data->action_bar, BUTTON_ID_UP);
      if (data->epochs_ms[CurrentTime] == 0) {
        action_bar_layer_clear_icon(data->action_bar, BUTTON_ID_DOWN);
      } else {
        action_bar_layer_set_icon(data->action_bar, BUTTON_ID_DOWN, data->icon_reset);
      }
    }
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing
//

// layer drawing callback
static void prv_layer_draw(Layer *layer, GContext *ctx) {
  // get WindowData pointer from layer data
  WindowData *data = (*(WindowData**)layer_get_data(layer));

  const GRect bounds = layer_get_bounds(layer);

  // get duration
  int64_t duration = prv_get_epoch_ms();
  if (data->epochs_ms[CurrentTime] != 0 && data->epochs_ms[CurrentTime] != duration) {
    duration -= (data->epochs_ms[CurrentTime] + duration) % duration;
  }
  else {
    duration = 0;
  }
  // format to text
  char buff_main[15];
  char buff_ms[15];
  prv_format_duration_ms(buff_main, buff_ms, sizeof(buff_main), sizeof(buff_ms), duration, false);
  // get coordinates and scaled sizes
  int32_t screen_width = bounds.size.w - ACTION_BAR_WIDTH;
  int32_t main_max_width = rendering_get_size(buff_main, 255);
  int32_t ms_max_width = rendering_get_size(buff_ms, 255);
  int32_t main_font_size;
  int32_t ms_font_size;
  if (strlen(buff_main) <= 3) {
    main_font_size = ms_font_size = 255 * (screen_width - SCREEN_BORDER_WIDTH * 2 -
      SCREEN_SPACING_WIDTH) / (main_max_width + ms_max_width);
  }
  else {
    ms_font_size = 11;
    main_font_size = 255 * ((screen_width - SCREEN_BORDER_WIDTH * 2 -
      SCREEN_SPACING_WIDTH) - rendering_get_size(buff_ms, ms_font_size)) /
      (main_max_width);
  }

  // animate total_height_ani, the combined height of all items
  // this is then used to center everything vertically
  data->total_height_ani += ((data->lap_count * SCREEN_LAP_HEIGHT + main_font_size +
    data->total_height_off) - data->total_height_ani) / 2;
  // animate the scrolling offset to give a bounce effect and prevent over-scrolling
  data->total_height_off -= prv_get_scrolling_start_offset(data) * 3 / 4;
  data->total_height_off += prv_get_scrolling_end_offset(data) * 3 / 4;

  // calculate screen coordinates
  int32_t main_width = rendering_get_size(buff_main, main_font_size);
  int32_t ms_width = rendering_get_size(buff_ms, ms_font_size);
  int32_t x_offset = (screen_width - (main_width + ms_width + SCREEN_SPACING_WIDTH)) / 2
                     + PBL_IF_RECT_ELSE(0, 10);
  int32_t y_offset = bounds.size.h / 2 - data->total_height_ani / 2;
  // lock main text position once laps go off screen
  if (prv_get_scrolling_enabled(data)) {
    y_offset = PBL_IF_RECT_ELSE(15, 43);
    if (data->total_height_off > 0) {
      data->total_height_off = 0;
    }
  }

  // draw laps
  graphics_context_set_text_color(ctx, GColorBlack);
  char buff_lap[15];
  for (int32_t ii = data->lap_count + 1, y = y_offset + data->total_height_ani;
    ii > 1;
    ii--, y -= SCREEN_LAP_HEIGHT) {
    // print to string
    prv_format_duration_ms(buff_lap, NULL, sizeof(buff_lap), 0, data->epochs_ms[ii], true);
    // draw on screen
    graphics_draw_text(ctx, buff_lap, data->font_small,
                       GRect(PBL_IF_RECT_ELSE(5, 15), y - 8, screen_width, SCREEN_LAP_HEIGHT),
                       GTextOverflowModeFill,
                       PBL_IF_RECT_ELSE(GTextAlignmentLeft, GTextAlignmentCenter), NULL);
  }

#if defined(PBL_ROUND)
    // Draw a box under the laps to cover the bottom laps
    graphics_context_set_fill_color(ctx, HIGHLIGHT_COLOR);
    graphics_fill_rect(ctx, GRect(0, bounds.size.h-24, 180, 24), 0, GCornerNone);
#endif

  // draw the text
  graphics_context_set_fill_color(ctx, HIGHLIGHT_COLOR);
  graphics_fill_rect(ctx,
                     GRect(0, 0, screen_width, y_offset + main_font_size + 5), 1, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  rendering_draw_text(ctx, buff_main, sizeof(buff_main), main_font_size,
    GPoint(x_offset, y_offset));
  rendering_draw_text(ctx, buff_ms, sizeof(buff_ms), ms_font_size,
    GPoint(x_offset + main_width + SCREEN_SPACING_WIDTH, y_offset));

  // draw time
  time_t tmp_sec = time(NULL);
  struct tm *tick_time = localtime(&tmp_sec);
  char buff[] = "00:00";
  if (clock_is_24h_style()) {
    strftime(buff, sizeof(buff), "%H:%M", tick_time);
  }
  else {
    strftime(buff, sizeof(buff), "%l:%M", tick_time);
  }
  const int time_y_offset = PBL_IF_RECT_ELSE(-3, 1);
  const int time_screen_width = PBL_IF_RECT_ELSE(screen_width, bounds.size.w);
  graphics_draw_text(ctx, buff, fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(0, time_y_offset, time_screen_width, 20), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Callbacks
//

// timer callback
static int32_t old_ani = 0, old_off = 0;
static void prv_app_timer_callback(void *callback_data) {
  WindowData *data = (WindowData*)callback_data;
  // refresh
  layer_mark_dirty(data->drawing_layer);

  // check if in fast refresh mode
  uint16_t refresh_ms = 80;
  if (old_ani == data->total_height_ani && old_off == data->total_height_off &&
      data->epochs_ms[CurrentTime] <= 0) {
    refresh_ms = 1000;
  }
  old_ani = data->total_height_ani;
  old_off = data->total_height_off;

  // schedule next refresh
  data->app_timer = app_timer_register(refresh_ms, prv_app_timer_callback, data);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Click Handlers
//

// button clicks
static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  WindowData *data = (WindowData*)context;
  // check if paused
  if (data->epochs_ms[CurrentTime] > 0) {
    // no repeating laps
    if (click_number_of_clicks_counted(recognizer) > 1) {
      return;
    }
    // move other lap's memory, overwriting the oldest
    memmove(&data->epochs_ms[FirstLapHistory + 1], &data->epochs_ms[FirstLapHistory],
      sizeof(data->epochs_ms[CurrentTime]) * (ARRAY_LENGTH(data->epochs_ms) - 3));
    // lap math
    int64_t cur_epoch_ms = prv_get_epoch_ms();
    data->epochs_ms[FirstLapHistory] = cur_epoch_ms - (data->epochs_ms[CurrentLap] + cur_epoch_ms) %
      cur_epoch_ms;
    data->epochs_ms[CurrentLap] = cur_epoch_ms;
    if (++data->lap_count > DATA_MAX_LAP_COUNT) {
      data->lap_count = DATA_MAX_LAP_COUNT;
      data->total_height_ani -= SCREEN_LAP_HEIGHT;
    }
    // scroll to top
    data->total_height_off = 0;
  }
  else if (prv_get_scrolling_enabled(data)) {
    if (click_number_of_clicks_counted(recognizer) > 1) {
      data->total_height_off += SCREEN_SCROLL_STEP_SMALL;
    }
    else {
      data->total_height_off += SCREEN_SCROLL_STEP_LARGE;
    }
  }

  // refresh
  app_timer_reschedule(data->app_timer, 10);
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  WindowData *data = (WindowData*)context;
  // play or pause the stopwatch
  if (data->epochs_ms[CurrentTime] <= 0) {
    // un-pause the stopwatch
    data->epochs_ms[CurrentTime] += prv_get_epoch_ms();
    data->epochs_ms[CurrentLap] += prv_get_epoch_ms();
  }
  else {
    // pause the stopwatch
    data->epochs_ms[CurrentTime] -= prv_get_epoch_ms();
    data->epochs_ms[CurrentLap] -= prv_get_epoch_ms();
  }
  prv_update_icons(data);

  // refresh
  app_timer_reschedule(data->app_timer, 10);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  WindowData *data = (WindowData*)context;
  // check if paused
  if (data->epochs_ms[CurrentTime] > 0 || !prv_get_scrolling_enabled(data)) {
    // exit if no time
    if (data->epochs_ms[CurrentTime] == 0) {
      return;
    }

    // reset stopwatch
    data->epochs_ms[CurrentTime] = 0;
    data->epochs_ms[CurrentLap] = 0;
    data->lap_count = 0;
    data->total_height_ani = 0;
    data->total_height_off = 0;
    prv_update_icons(data);
  }
  else {
    if (click_number_of_clicks_counted(recognizer) > 1) {
      data->total_height_off -= SCREEN_SCROLL_STEP_SMALL;
    }
    else {
      data->total_height_off -= SCREEN_SCROLL_STEP_LARGE;
    }
  }

  // refresh
  app_timer_reschedule(data->app_timer, 10);
}

// click configuration provider
static void prv_click_config_provider(void *context) {
  window_set_click_context(BUTTON_ID_UP, context);
  window_set_click_context(BUTTON_ID_SELECT, context);
  window_set_click_context(BUTTON_ID_DOWN, context);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 50, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 50, prv_down_click_handler);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Loading/Unloading
//

// update app glance
static void prv_update_app_glance(AppGlanceReloadSession *session, size_t limit, void *context) {
  // Ensure we have sufficient slices
  if (limit < 1) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error: app needs %d slices (%zu available)", 1, limit);
    return;
  }

  WindowData *data = context;

  // Stopped -> no Status
  if(data->epochs_ms[CurrentTime] == 0){
    return;
  }

  time_t expiration_time = APP_GLANCE_SLICE_NO_EXPIRATION;
  char buff_glance[50] = {0};

  // Running
  if(data->epochs_ms[CurrentTime] > 0){
    snprintf(buff_glance, sizeof(buff_glance), "{time_since(%lld)|format('%%0fR:%%0S')}", data->epochs_ms[CurrentTime]/1000);
  }
  // Paused
  else if(data->epochs_ms[CurrentTime] < 0){
    // expiration if paused > 2 hours
    expiration_time = time(NULL) + SECONDS_PER_HOUR * 2; 

    // get duration
    int64_t duration = prv_get_epoch_ms();
    duration -= (data->epochs_ms[CurrentTime] + duration) % duration;

    // format to text
    char buff_main[15];
    char buff_ms[15];
    prv_format_duration_ms(buff_main, buff_ms, sizeof(buff_main), sizeof(buff_ms), duration, false);
    snprintf(buff_glance, sizeof(buff_glance), "%s%s%s", buff_main, strlen(buff_main)>3 ? "." : "", buff_ms);
  }

  // Create the slice
  const AppGlanceSlice slice = {
    .layout = {
      .subtitle_template_string = buff_glance
    },
    .expiration_time = expiration_time
  };

  // add the slice, and check the result
  AppGlanceResult result = app_glance_add_slice(session, slice);
  if (result != APP_GLANCE_RESULT_SUCCESS) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error adding AppGlanceSlice: %d", result);
  }
}

// save persistent storage
static void prv_save_persistent_storage(WindowData *data) {
  // write out data and storage version
  persist_write_int(PERSIST_DATA_VERSION_KEY, PERSIST_DATA_VERSION);
  int32_t data_key = PERSIST_DATA_KEY;
  persist_write_int(data_key++, data->lap_count);
  persist_write_int(data_key++, data->total_height_ani);
  persist_write_data(data_key, data->epochs_ms, sizeof(data->epochs_ms));
}

// load persistent storage
static void prv_load_persistent_storage(WindowData *data) {
  // check if data and get version
  if (persist_exists(PERSIST_DATA_VERSION_KEY)) {
    int32_t version = persist_read_int(PERSIST_DATA_VERSION_KEY);
    int32_t data_key = PERSIST_DATA_KEY;
    // load data with that version format
    switch (version) {
      case 1:
        // load data version one and exit
        data->lap_count = persist_read_int(data_key++);
        data->total_height_ani = persist_read_int(data_key++);
        persist_read_data(data_key, data->epochs_ms, sizeof(data->epochs_ms));
        return;
      default:
        // if it doesn't match a known data version, delete it and start fresh
        persist_delete(PERSIST_DATA_VERSION_KEY);
        persist_delete(PERSIST_DATA_KEY);
        break;
    }
  }
  // error handling
  // if no know data version or first time loading reset all data
  memset(data->epochs_ms, 0, sizeof(data->epochs_ms));
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Unrecognized or no persistent storage");
}

// load the window
static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  // create window data
  WindowData *data = (WindowData*)malloc(sizeof(WindowData));
  window_set_user_data(window, data);
  if (data) {
    // load persistent state
    prv_load_persistent_storage(data);
    // load resources
    data->font_small = fonts_load_custom_font(
      resource_get_handle(RESOURCE_ID_FONT_LECO_REGULAR_SUBSET_16));
    data->icon_down = gbitmap_create_with_resource(RESOURCE_ID_ICON_DOWN);
    data->icon_lap = gbitmap_create_with_resource(RESOURCE_ID_ICON_LAP);
    data->icon_pause = gbitmap_create_with_resource(RESOURCE_ID_ICON_PAUSE);
    data->icon_play = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLAY);
    data->icon_reset = gbitmap_create_with_resource(RESOURCE_ID_ICON_RESET);
    data->icon_up = gbitmap_create_with_resource(RESOURCE_ID_ICON_UP);
    // create layer
    // IMPORTANT: must be created with data for the WindowData pointer
    // so that the data can be accessed in the layer_update_proc callback
    data->drawing_layer = layer_create_with_data(bounds, sizeof(WindowData*));
    WindowData **layer_data = (WindowData**)layer_get_data(data->drawing_layer);
    (*layer_data) = data;
    layer_set_update_proc(data->drawing_layer, prv_layer_draw);
    layer_add_child(root, data->drawing_layer);
    // create action bar
    data->action_bar = action_bar_layer_create();
    action_bar_layer_set_context(data->action_bar, data);
    action_bar_layer_set_click_config_provider(data->action_bar, prv_click_config_provider);
    prv_update_icons(data);
    if (data->lap_count > SCREEN_MAX_LAPS_BEFORE_SCROLLING && data->epochs_ms[CurrentTime] < 0) {
      action_bar_layer_set_icon(data->action_bar, BUTTON_ID_UP, data->icon_up);
      action_bar_layer_set_icon(data->action_bar, BUTTON_ID_DOWN, data->icon_down);
    }
    action_bar_layer_add_to_window(data->action_bar, window);

    // schedule refresh timer
    app_timer_register(70, prv_app_timer_callback, data);
    return;
  }

  // error handling
  window_stack_remove(window, false);
  APP_LOG(APP_LOG_LEVEL_ERROR, "Exiting: Failed to allocate WindowData");
}

// unload the window
static void prv_window_unload(Window *window) {
  WindowData *data = (WindowData*)window_get_user_data(window);
  // free memory
  if (data) {
    // save persistent storage
    prv_save_persistent_storage(data);
    // update appglance
    app_glance_reload(prv_update_app_glance, data);
    // destroy visuals
    action_bar_layer_destroy(data->action_bar);
    layer_destroy(data->drawing_layer);
    // unload resources
    fonts_unload_custom_font(data->font_small);
    gbitmap_destroy(data->icon_down);
    gbitmap_destroy(data->icon_lap);
    gbitmap_destroy(data->icon_pause);
    gbitmap_destroy(data->icon_play);
    gbitmap_destroy(data->icon_reset);
    gbitmap_destroy(data->icon_up);
    // window
    window_destroy(window);
    free(data);
    return;
  }

  // error handling
  window_destroy(window);
  APP_LOG(APP_LOG_LEVEL_ERROR, "Error: Tried to free NULL WindowData");
}

// load the window
static void prv_initialize(void) {
  Window *window = window_create();
#ifdef PBL_SDK_3
  window_set_background_color(window, HIGHLIGHT_COLOR);
#else
  window_set_fullscreen(window, true);
#endif
  window_set_window_handlers(window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(window, true);
}

// main entry
int main(void) {
  prv_initialize();
  app_event_loop();
}
