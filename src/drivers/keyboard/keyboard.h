#ifndef MYOS_KEYBOARD_H
#define MYOS_KEYBOARD_H

#include <stdbool.h>

void keyboard_init(void);
void keyboard_handler(void);
void keyboard_set_user_mode(bool active);
bool keyboard_read_user_char(char *character);
void keyboard_process_pending(void);
void keyboard_wait_for_event(void);

#endif
