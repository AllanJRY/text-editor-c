/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** datas ***/

struct termios original_termios;

/*** terminal ***/

void die(const char* s) {
    perror(s);
    exit(1);
}

void disable_raw_mode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios) == -1) {
        die("`tcsetattr` fail when disabling raw mode");
    }
}

void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        die("`tcgetattr` fail when enabling raw mode");
    }
    atexit(disable_raw_mode);

    struct termios raw = original_termios;

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

/*** init ***/

int main(void) {
    enable_raw_mode();

    while (1) {
        char c = '\0';
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
            die("Error while reading input");
        }

        if(iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q') break; 
    }
    return 0;
}

