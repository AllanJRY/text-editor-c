/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define EDITOR_VERSION "0.0.1"

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

    while ((read_count = read(STDIN_FILENO, &c, 1)) != 1) {
        if (read_count == -1 && errno != EAGAIN) {
            die("Error while reading input");
        }
    }

    return c;
}

bool get_cursor_position(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return false;
    }

    while(i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }

        i += 1;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') {
        die("wesh");
        return false;
    }

    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return false;
    }

    return true;
}

bool get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return false;
        }

        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return true;
    }
}

/*** append buffer ***/

typedef struct Append_Buf {
    char* b;
    int len;
} Append_Buf;

#define APPEND_BUF_INIT { .b = NULL, .len = 0 }

void append_buf_append(Append_Buf* buf, const char* s, int len) {
    char *new = realloc(buf->b, buf->len + len);

    if (new == NULL) return;

    memcpy(&new[buf->len], s, len);
    buf->b    = new;
    buf->len += len;
}

void append_buf_free(Append_Buf* buf) {
    free(buf->b);
}


/*** output ***/

void editor_draw_rows(Append_Buf* buf) {
    for(int y = 0; y < editor_state.screen_rows; y++) {
        if(y == editor_state.screen_rows / 3) {
            char welcome[80];
            int welcome_len = snprintf(welcome, sizeof(welcome), "Editeur -- version %s", EDITOR_VERSION);

            if (welcome_len > editor_state.screen_cols) {
                welcome_len = editor_state.screen_cols;
            }

            int padding = (editor_state.screen_cols - welcome_len) / 2;
            if (padding) {
                append_buf_append(buf, "~", 1);
            }

            while (padding -= 1) {
                append_buf_append(buf, " ", 1);
            }

            append_buf_append(buf, welcome, welcome_len);
        }
        else {
            append_buf_append(buf, "~", 1);
        }

        // clear the current line.
        append_buf_append(buf, "\x1b[K", 3);

        if (y < editor_state.screen_rows - 1) {
            append_buf_append(buf, "\r\n", 2);
        }
    }
}

void editor_refresh_screen(void) {
    Append_Buf buf = APPEND_BUF_INIT;
    
    // hide the cursor
    append_buf_append(&buf, "\x1b[?25l", 6);

    // clear the entire screen, not used because each line is cleared in editor_draw_rows.
    // append_buf_append(&buf, "\x1b[2J", 4);

    // move the cursor to the top-left corner.
    append_buf_append(&buf, "\x1b[H", 3);

    editor_draw_rows(&buf);

    append_buf_append(&buf, "\x1b[H", 3);

    // show the cursor
    append_buf_append(&buf, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buf.b, buf.len);
    append_buf_free(&buf);
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

