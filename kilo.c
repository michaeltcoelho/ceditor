#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct editorConfig {
    struct termios originalTermAttrs;
};

struct editorConfig state;

void clearEditorScreen();

void die(const char *s) {
    clearEditorScreen();
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.originalTermAttrs) == -1) die("tcsetattr");
}

// enable raw mode
void enableCustomTerminalMode() {
    // read terminal settings into "originalTermAttrs"
    if(tcgetattr(STDIN_FILENO, &state.originalTermAttrs) == -1) die("tcgetattr");

    // rollback terminal changes when program exits
    atexit(disableRawMode);

    struct termios termAttrs = state.originalTermAttrs;
    // disable ctrl+s/ctrl+q freezing data from being transmitted to the terminal
    termAttrs.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // disable output processing like: `\n` to `\r\n`. `\r` carriage return, it moves cursor to the begining of the current line
    termAttrs.c_oflag &= ~(OPOST);
    // disable miscellaneous flags
    termAttrs.c_cflag |= (CS8);
    // disable echoing mode, canonical mode and signint and also sigtstp
    termAttrs.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // read() returns if it doesnt get any input for a certain amount of time
    termAttrs.c_cc[VMIN] = 0; // min. number of bytes input needed before read() can return
    termAttrs.c_cc[VTIME] = 1; // max. amount of time to wait before read() returns

    // apply changes to the terminal
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &termAttrs) == -1) die("tcsetattr");
}

char readKeysFromInput() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
}

void mapEditorKeys() {
    char c = readKeysFromInput();
    switch (c) {
        case CTRL_KEY('q'):
            clearEditorScreen();
            exit(0);
            break;
    }
}

void drawTildesRows() {
    int y;
    for (y = 0; y < 24; y++) {
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void clearEditorScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void refreshEditorScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    drawTildesRows();

    write(STDOUT_FILENO, "\x1b[H", 3);
}

int main() {
    enableCustomTerminalMode();

    while (1) {
        refreshEditorScreen();
        mapEditorKeys();
    }

    return 0;
}
