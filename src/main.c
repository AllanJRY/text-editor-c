/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** datas ***/

typedef struct Editor_State {
    int screen_rows;
    int screen_cols;
    struct termios original_termios;
} Editor_State;

Editor_State editor_state;

/*** terminal ***/

void die(const char* s) {
    // clear the entire screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // move the cursor to the top-left corner.
    write(STDOUT_FILENO, "\x1b[H", 4);

    perror(s);
    exit(1);
}

void disable_raw_mode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor_state.original_termios) == -1) {
        die("`tcsetattr` fail when disabling raw mode");
    }
}

void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &editor_state.original_termios) == -1) {
        die("`tcgetattr` fail when enabling raw mode");
    }
    atexit(disable_raw_mode);

    struct termios raw = editor_state.original_termios;

    // ICRNL Fix Ctrl-M, prevent carriage return `\r` (13) to be transtaled to `\n` (10).
    // IXON disable Ctrl+S and Ctrl+Q.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Torn off output processing. This prevent `\n` to be translated to `\r\n`.
    raw.c_oflag &= ~(OPOST);

    raw.c_cflag |= (CS8);

    // ICANON ENABLE read byte by byte, no need to wait for EOF char.
    // IEXTEN disable Ctrl+V.
    // ISIG disable Ctrl+C and Ctrl+Z.
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);


    // Sets the minimum number of bytes of input needed before read() can return.
    raw.c_cc[VMIN] = 0;
    // Sets the maximum amount of time to wait before read() returns (is is in tenths of a second).
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("`tcsetattr` fail when enabling raw mode");
    }
}

char editor_read_key(void) {
    int read_count;
    char c;

    while ((read_count = read(STDIN_FILENO, &c, 1)) == -1) {
        if (read_count == -1 && errno != EAGAIN) {
            die("Error while reading input");
        }
    }

    return c;
}

// TODO: could return bool ?
bool get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return false;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return true;
    }
}

/*** output ***/

void editor_draw_rows(void) {
    for(int y = 0; y < editor_state.screen_rows; y++) {
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void editor_refresh_screen(void) {
    // clear the entire screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // move the cursor to the top-left corner.
    write(STDOUT_FILENO, "\x1b[H", 4);

    editor_draw_rows();

    write(STDOUT_FILENO, "\x1b[H", 4);
}

/*** input ***/

void editor_process_keypress(void) {
    char c = editor_read_key();

    switch(c) {
        case CTRL_KEY('q'):
            editor_refresh_screen();
            exit(0);
            break;
    }
}

/*** init ***/

void init_editor(void) {
    if(!get_window_size(&editor_state.screen_rows, &editor_state.screen_cols)) {
        die("Error during editor init");
    }
}

int main(void) {
    enable_raw_mode();
    init_editor();

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}

