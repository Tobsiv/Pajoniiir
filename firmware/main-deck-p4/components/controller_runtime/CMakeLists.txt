idf_component_register(
    SRCS "flx4_map.c" "controller_event_buffer.c" "controller_runtime.c"
         "usb_midi_codec.c" "flx4_led_midi.c"
    INCLUDE_DIRS "include"
    REQUIRES controller_profile controller_profile_runtime
             control_link control_state_reconciler
)
