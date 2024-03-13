#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/
#define TON_VERSION "0.0.1"
#define TON_TAB_STOP 8
#define TON_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenRows;
  int screenCols;
  int numRows;
  erow *row;
  int dirty;
  char *filename;
  char statusMsg[80];
  time_t statusMsgTime;
  struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMsg(const char *fmt, ...);

/*** terminal ***/
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if(tcsetattr(STDIN_FILENO,TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() {
  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcsetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cflag |= ~(CS8);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if(nread == -1 && errno != EAGAIN) die("read");
  }

  if(c == '\x1b') {
    char seq[3];

    if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if(seq[0] == '[') {
      if(seq[1] >= '0' && seq[1] <= '9') {
        if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if(seq[2] == '~') {
          switch(seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch(seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if(seq[0] == 'O') {
      switch(seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }

}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if(write(STDOUT_FILENO,"\x1b[6n", 4) != 4) return -1;

  while(i < sizeof(buf) -1) {
    if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if(buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if(buf[0] != '\x1b' || buf[1] != '[') return -1;
  if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if(ioctl(STDIN_FILENO, TIOCSWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for(int i = 0; i < cx; i++) {
    if(row->chars[i] == '\t')
      rx += (TON_TAB_STOP - 1) - (rx % TON_TAB_STOP);
    rx++;
  }
  return rx;
} 

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for(j = 0; j < row->size; j++) 
    if(row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(TON_TAB_STOP - 1) + 1);

  int idx = 0;
  for(j = 0; j < row->size; j++) {
    if(row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while(idx % TON_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));

  int at = E.numRows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numRows++;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if(at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at+1],&row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if(at < 0 || at >= row->size) return;
  memmove(&E.row->chars[at], &row->chars[at+1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
  if(E.cy == E.numRows) {
    editorAppendRow("", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorDelChar() {
  if(E.cy == E.numRows) return;

  erow *row = &E.row[E.cy];
  if(E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  }
}

/*** file I/O ***/
char* editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for(j = 0; j < E.numRows; j++) totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for(j = 0; j < E.numRows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if(!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while((linelen = getline(&line, &linecap, fp)) != -1) {
    while(linelen > 0 && (line[linelen -1] == '\n' || line[linelen - 1] == '\r')) linelen--;
    editorAppendRow(line, linelen);
  }
  free(line); 
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if(E.filename == NULL) return;

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // O_CREAT - create if not exists | 0644 - is the default permission | O_RDWR - read an write
  if(fd != -1) {
    if(ftruncate(fd, len) != -1) {
      if(write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMsg("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  } 
  free(buf);
  editorSetStatusMsg("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len+len);

  if(new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/
void editorScroll() {
  E.rx = 0;
  if(E.cy < E.numRows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if(E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if(E.cy >= E.rowoff + E.screenRows) {
    E.rowoff = E.cy - E.screenRows + 1; 
  }
  if(E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if(E.rx >= E.coloff + E.screenCols) {
    E.coloff = E.rx - E.screenCols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for(y = 0; y < E.screenRows; y++) {
    int filerow = y + E.rowoff;
    if(filerow >= E.numRows) {
      if(E.numRows == 0 && y == E.screenRows / 3) {
        char welcome[80];
        int welcomeLen = snprintf(welcome, sizeof(welcome), "Ton Editor -- version %s", TON_VERSION);
        if(welcomeLen > E.screenCols) welcomeLen = E.screenCols;
        int padding = (E.screenCols - welcomeLen) / 2;
        if(padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while(padding--) abAppend(ab, "   ", 1);
        abAppend(ab, welcome, welcomeLen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if(len < 0) len = 0;
      if(len > E.screenCols) len = E.screenCols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
    
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), " %.20s - %d lines %s",
      E.filename ? E.filename : "[NO NAME]",
      E.numRows, E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numRows);
  if(len > E.screenCols) len = E.screenCols;
  abAppend(ab, status, len);
  while(len < E.screenCols) {
    if(E.screenCols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusMsg);
  if(msglen > E.screenCols) msglen = E.screenCols;
  if(msglen && time(NULL) - E.statusMsgTime < 5) abAppend(ab, E.statusMsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMsg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusMsg, sizeof(E.statusMsg), fmt, ap);
  va_end(ap);
  E.statusMsgTime = time(NULL);
}

/*** input ***/
void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if(E.cx != 0) {
        E.cx--;
      } else if(E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if(row && E.cx < row->size) {
        E.cx++;
      } else if(row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if(E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if(E.cy < E.numRows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
  int rowLen = row ? row->size : 0;
  if(E.cx > rowLen) {
    E.cx = rowLen;
  }
}

void editorProcessKeypress() {
  static int quitTimes = TON_QUIT_TIMES;
  int c = editorReadKey();
  switch(c) {
    case '\r':
      // TODO
      break;
    case CTRL_KEY('q'):
      if(E.dirty && quitTimes > 0) {
        editorSetStatusMsg("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit", quitTimes);
        quitTimes--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if(E.cy < E.numRows) E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if(c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN: {
      if(c == PAGE_UP) {
        E.cy = E.rowoff;
      } else if(c == PAGE_DOWN) {
        E.cy = E.rowoff + E.screenRows - 1;
        if(E.cy > E.numRows) E.cy = E.numRows;
      }
      
      int times = E.screenRows;
      while(times--) {
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l') :
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }

  quitTimes = TON_QUIT_TIMES;
}

/*** init ***/
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numRows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusMsg[0] = '\0';
  E.statusMsgTime = 0;

  if(getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
  E.screenRows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if(argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMsg("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while(1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}