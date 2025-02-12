#include "view_dispatcher_i.h"

#define TAG "ViewDispatcher"

ViewDispatcher* view_dispatcher_alloc(void) {
    ViewDispatcher* view_dispatcher = malloc(sizeof(ViewDispatcher));

    view_dispatcher->view_port = view_port_alloc();
    view_port_draw_callback_set(
        view_dispatcher->view_port, view_dispatcher_draw_callback, view_dispatcher);
    view_port_input_callback_set(
        view_dispatcher->view_port, view_dispatcher_input_callback, view_dispatcher);
    view_port_enabled_set(view_dispatcher->view_port, false);

    ViewDict_init(view_dispatcher->views);

    return view_dispatcher;
}

void view_dispatcher_free(ViewDispatcher* view_dispatcher) {
    // Detach from gui
    if(view_dispatcher->gui) {
        gui_remove_view_port(view_dispatcher->gui, view_dispatcher->view_port);
    }
    // Crash if not all views were freed
    furi_check(!ViewDict_size(view_dispatcher->views));

    ViewDict_clear(view_dispatcher->views);
    // Free ViewPort
    view_port_free(view_dispatcher->view_port);
    // Free internal queue
    if(view_dispatcher->input_queue) {
        furi_event_loop_message_queue_unsubscribe(
            view_dispatcher->event_loop, view_dispatcher->input_queue);
        furi_message_queue_free(view_dispatcher->input_queue);
    }
    if(view_dispatcher->event_queue) {
        furi_event_loop_message_queue_unsubscribe(
            view_dispatcher->event_loop, view_dispatcher->event_queue);
        furi_message_queue_free(view_dispatcher->event_queue);
    }
    if(view_dispatcher->event_loop) {
        furi_event_loop_free(view_dispatcher->event_loop);
    }
    // Free dispatcher
    free(view_dispatcher);
}

void view_dispatcher_enable_queue(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->event_loop == NULL);

    view_dispatcher->event_loop = furi_event_loop_alloc();

    view_dispatcher->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    furi_event_loop_message_queue_subscribe(
        view_dispatcher->event_loop,
        view_dispatcher->input_queue,
        FuriEventLoopEventIn,
        view_dispatcher_run_input_callback,
        view_dispatcher);

    view_dispatcher->event_queue = furi_message_queue_alloc(16, sizeof(uint32_t));
    furi_event_loop_message_queue_subscribe(
        view_dispatcher->event_loop,
        view_dispatcher->event_queue,
        FuriEventLoopEventIn,
        view_dispatcher_run_event_callback,
        view_dispatcher);
}

void view_dispatcher_set_navigation_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherNavigationEventCallback callback) {
    furi_check(view_dispatcher);
    view_dispatcher->navigation_event_callback = callback;
}

void view_dispatcher_set_custom_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherCustomEventCallback callback) {
    furi_check(view_dispatcher);
    view_dispatcher->custom_event_callback = callback;
}

void view_dispatcher_set_tick_event_callback(
    ViewDispatcher* view_dispatcher,
    ViewDispatcherTickEventCallback callback,
    uint32_t tick_period) {
    furi_check(view_dispatcher);
    view_dispatcher->tick_event_callback = callback;
    view_dispatcher->tick_period = tick_period;
}

void view_dispatcher_set_event_callback_context(ViewDispatcher* view_dispatcher, void* context) {
    furi_check(view_dispatcher);
    view_dispatcher->event_context = context;
}

FuriEventLoop* view_dispatcher_get_event_loop(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->event_loop);

    return view_dispatcher->event_loop;
}

void view_dispatcher_run(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->event_loop);

    uint32_t tick_period = view_dispatcher->tick_period == 0 ? FuriWaitForever :
                                                               view_dispatcher->tick_period;

    furi_event_loop_tick_set(
        view_dispatcher->event_loop,
        tick_period,
        view_dispatcher_handle_tick_event,
        view_dispatcher);

    furi_event_loop_run(view_dispatcher->event_loop);

    // Wait till all input events delivered
    InputEvent input;
    while(view_dispatcher->ongoing_input) {
        furi_message_queue_get(view_dispatcher->input_queue, &input, FuriWaitForever);
        uint8_t key_bit = (1 << input.key);
        if(input.type == InputTypePress) {
            view_dispatcher->ongoing_input |= key_bit;
        } else if(input.type == InputTypeRelease) {
            view_dispatcher->ongoing_input &= ~key_bit;
        }
    }
}

void view_dispatcher_stop(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->event_loop);
    furi_event_loop_stop(view_dispatcher->event_loop);
}

void view_dispatcher_add_view(ViewDispatcher* view_dispatcher, uint32_t view_id, View* view) {
    furi_check(view_dispatcher);
    furi_check(view);
    // Check if view id is not used and register view
    furi_check(ViewDict_get(view_dispatcher->views, view_id) == NULL);

    // Lock gui
    if(view_dispatcher->gui) {
        gui_lock(view_dispatcher->gui);
    }

    ViewDict_set_at(view_dispatcher->views, view_id, view);
    view_set_update_callback(view, view_dispatcher_update);
    view_set_update_callback_context(view, view_dispatcher);

    // Unlock gui
    if(view_dispatcher->gui) {
        gui_unlock(view_dispatcher->gui);
    }
}

void view_dispatcher_remove_view(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_check(view_dispatcher);

    // Lock gui
    if(view_dispatcher->gui) {
        gui_lock(view_dispatcher->gui);
    }
    // Get View by ID
    View* view = *ViewDict_get(view_dispatcher->views, view_id);

    // Disable the view if it is active
    if(view_dispatcher->current_view == view) {
        view_dispatcher_set_current_view(view_dispatcher, NULL);
    }
    // Check if view is receiving input
    if(view_dispatcher->ongoing_input_view == view) {
        view_dispatcher->ongoing_input_view = NULL;
    }
    // Remove view
    furi_check(ViewDict_erase(view_dispatcher->views, view_id));

    view_set_update_callback(view, NULL);
    view_set_update_callback_context(view, NULL);

    // Unlock gui
    if(view_dispatcher->gui) {
        gui_unlock(view_dispatcher->gui);
    }
}

void view_dispatcher_switch_to_view(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_check(view_dispatcher);
    if(view_id == VIEW_NONE) {
        view_dispatcher_set_current_view(view_dispatcher, NULL);
    } else if(view_id == VIEW_IGNORE) {
    } else {
        View** view_pp = ViewDict_get(view_dispatcher->views, view_id);
        furi_check(view_pp != NULL);
        view_dispatcher_set_current_view(view_dispatcher, *view_pp);
    }
}

void view_dispatcher_send_to_front(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui);
    gui_view_port_send_to_front(view_dispatcher->gui, view_dispatcher->view_port);
}

void view_dispatcher_send_to_back(ViewDispatcher* view_dispatcher) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui);
    gui_view_port_send_to_back(view_dispatcher->gui, view_dispatcher->view_port);
}

void view_dispatcher_attach_to_gui(
    ViewDispatcher* view_dispatcher,
    Gui* gui,
    ViewDispatcherType type) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->gui == NULL);
    furi_check(gui);

    if(type == ViewDispatcherTypeDesktop) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerDesktop);
    } else if(type == ViewDispatcherTypeWindow) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerWindow);
    } else if(type == ViewDispatcherTypeFullscreen) {
        gui_add_view_port(gui, view_dispatcher->view_port, GuiLayerFullscreen);
    } else {
        furi_crash();
    }
    view_dispatcher->gui = gui;
}

void view_dispatcher_draw_callback(Canvas* canvas, void* context) {
    ViewDispatcher* view_dispatcher = context;
    if(view_dispatcher->current_view) {
        view_draw(view_dispatcher->current_view, canvas);
    }
}

void view_dispatcher_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* view_dispatcher = context;
    if(view_dispatcher->input_queue) {
        furi_check(
            furi_message_queue_put(view_dispatcher->input_queue, event, FuriWaitForever) ==
            FuriStatusOk);
    } else {
        view_dispatcher_handle_input(view_dispatcher, event);
    }
}

void view_dispatcher_handle_input(ViewDispatcher* view_dispatcher, InputEvent* event) {
    // Check input complementarity
    uint8_t key_bit = (1 << event->key);
    if(event->type == InputTypePress) {
        view_dispatcher->ongoing_input |= key_bit;
    } else if(event->type == InputTypeRelease) {
        view_dispatcher->ongoing_input &= ~key_bit;
    } else if(!(view_dispatcher->ongoing_input & key_bit)) {
        FURI_LOG_D(
            TAG,
            "non-complementary input, discarding key: %s, type: %s, sequence: %p",
            input_get_key_name(event->key),
            input_get_type_name(event->type),
            (void*)event->sequence);
        return;
    }

    // Set ongoing input view if this is event is first press event
    if(!(view_dispatcher->ongoing_input & ~key_bit) && event->type == InputTypePress) {
        view_dispatcher->ongoing_input_view = view_dispatcher->current_view;
    }

    // Deliver event
    if(view_dispatcher->current_view &&
       view_dispatcher->ongoing_input_view == view_dispatcher->current_view) {
        // Dispatch input to current view
        bool is_consumed = view_input(view_dispatcher->current_view, event);

        // Navigate if input is not consumed
        if(!is_consumed && (event->key == InputKeyBack) &&
           (event->type == InputTypeShort || event->type == InputTypeLong)) {
            // Navigate to previous
            uint32_t view_id = view_previous(view_dispatcher->current_view);
            if(view_id != VIEW_IGNORE) {
                // Switch to returned view
                view_dispatcher_switch_to_view(view_dispatcher, view_id);
            } else if(view_dispatcher->navigation_event_callback) {
                // Dispatch navigation event
                if(!view_dispatcher->navigation_event_callback(view_dispatcher->event_context)) {
                    view_dispatcher_stop(view_dispatcher);
                    return;
                }
            }
        }
    } else if(view_dispatcher->ongoing_input_view && event->type == InputTypeRelease) {
        FURI_LOG_D(
            TAG,
            "View changed while key press %p -> %p. Sending key: %s, type: %s, sequence: %p to previous view",
            view_dispatcher->ongoing_input_view,
            view_dispatcher->current_view,
            input_get_key_name(event->key),
            input_get_type_name(event->type),
            (void*)event->sequence);
        view_input(view_dispatcher->ongoing_input_view, event);
    }
}

void view_dispatcher_handle_tick_event(void* context) {
    ViewDispatcher* view_dispatcher = context;
    if(view_dispatcher->tick_event_callback) {
        view_dispatcher->tick_event_callback(view_dispatcher->event_context);
    }
}

void view_dispatcher_handle_custom_event(ViewDispatcher* view_dispatcher, uint32_t event) {
    bool is_consumed = false;
    if(view_dispatcher->current_view) {
        is_consumed = view_custom(view_dispatcher->current_view, event);
    }
    // If custom event is not consumed in View, call callback
    if(!is_consumed && view_dispatcher->custom_event_callback) {
        view_dispatcher->custom_event_callback(view_dispatcher->event_context, event);
    }
}

void view_dispatcher_send_custom_event(ViewDispatcher* view_dispatcher, uint32_t event) {
    furi_check(view_dispatcher);
    furi_check(view_dispatcher->event_loop);

    furi_check(
        furi_message_queue_put(view_dispatcher->event_queue, &event, FuriWaitForever) ==
        FuriStatusOk);
}

static const ViewPortOrientation view_dispatcher_view_port_orientation_table[] = {
    [ViewOrientationVertical] = ViewPortOrientationVertical,
    [ViewOrientationVerticalFlip] = ViewPortOrientationVerticalFlip,
    [ViewOrientationHorizontal] = ViewPortOrientationHorizontal,
    [ViewOrientationHorizontalFlip] = ViewPortOrientationHorizontalFlip,
};

void view_dispatcher_set_current_view(ViewDispatcher* view_dispatcher, View* view) {
    furi_check(view_dispatcher);
    // Dispatch view exit event
    if(view_dispatcher->current_view) {
        view_exit(view_dispatcher->current_view);
    }
    // Set current view
    view_dispatcher->current_view = view;
    // Dispatch view enter event
    if(view_dispatcher->current_view) {
        ViewPortOrientation orientation =
            view_dispatcher_view_port_orientation_table[view->orientation];
        if(view_port_get_orientation(view_dispatcher->view_port) != orientation) {
            view_port_set_orientation(view_dispatcher->view_port, orientation);
            // we just rotated input keys, now it's time to sacrifice some input
            view_dispatcher->ongoing_input = 0;
        }
        view_enter(view_dispatcher->current_view);
        view_port_enabled_set(view_dispatcher->view_port, true);
        view_port_update(view_dispatcher->view_port);
    } else {
        view_port_enabled_set(view_dispatcher->view_port, false);
        if(view_dispatcher->event_loop) {
            view_dispatcher_stop(view_dispatcher);
        }
    }
}

void view_dispatcher_update(View* view, void* context) {
    furi_check(view);
    furi_check(context);

    ViewDispatcher* view_dispatcher = context;

    if(view_dispatcher->current_view == view) {
        view_port_update(view_dispatcher->view_port);
    }
}

bool view_dispatcher_run_event_callback(FuriMessageQueue* queue, void* context) {
    furi_assert(context);
    ViewDispatcher* instance = context;
    furi_assert(instance->event_queue == queue);

    uint32_t event;
    furi_check(furi_message_queue_get(instance->event_queue, &event, 0) == FuriStatusOk);
    view_dispatcher_handle_custom_event(instance, event);

    return true;
}

bool view_dispatcher_run_input_callback(FuriMessageQueue* queue, void* context) {
    furi_assert(context);
    ViewDispatcher* instance = context;
    furi_assert(instance->input_queue == queue);

    InputEvent input;
    furi_check(furi_message_queue_get(instance->input_queue, &input, 0) == FuriStatusOk);
    view_dispatcher_handle_input(instance, &input);

    return true;
}
