#ifndef DISPLAY_H
#define DISPLAY_H

/* The game window (SDL2), or headless frame dumps for testing. */
void display_init(int headless, const char *dump_dir);
void display_open(void);
/* Show what the GS displays now and wait for the next 1/59.94 s field. */
void display_vsync(void);
/* The game stopped (unimplemented function): keep the last picture up until the window
   is closed; headless, write it to the dump folder. */
void display_hold(const char *why);

#endif
