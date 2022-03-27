#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

#define TAB_SIZE 4

#define CTRL_KEY(k) ((k) & 0x1f)

#define MSTRING_INIT {NULL, 0}

/* mutable string */
typedef struct mstring {
	char *buf;
	size_t len;
} mstring_t;

typedef struct row {
	char *buf;
	char *render;
	size_t size;
	size_t rsize;
} row_t;

HANDLE hstdin, hstdout, con1, con2;
DWORD old_mode, mode, hstdout_mode;
DWORD dwread, dwwritten;

CONSOLE_SCREEN_BUFFER_INFO csbi;
CONSOLE_CURSOR_INFO cci;

size_t cx, cy, rx = 0, rowoff = 0, coloff = 0;
size_t screenrows, screencols;

row_t *rows = NULL;
size_t nrows = 0;

char *curfile = NULL;
char *statusmsg = NULL;

int modified = 0;
int quitting = 0;

void mstr_append(mstring_t *ms, const char *s, size_t len) {
	char *newstr = (char*)realloc(ms->buf, ms->len + len);
	if (newstr == NULL) return;
	memcpy(&newstr[ms->len], s, len);
	ms->buf = newstr;
	ms->len += len;
}

void mstr_free(mstring_t *ms) {
	free(ms->buf);
}

void conprintf(HANDLE h, const char *format, ...) {
	va_list val;
	va_start(val, format);
	size_t size = vsnprintf(NULL, 0, format, val);
	char *buf = (char*)malloc((size + 1) * sizeof(char));
	if (vsnprintf(buf, size + 1, format, val) < 0) return;
	buf[size] = '\0';
	DWORD wr;
	WriteConsole(h, buf, size, &wr, NULL);
	va_end(val);
	free(buf);
}

size_t cx2rx(row_t *row, size_t x) {
	size_t r = 0;
	int j;
	for (j = 0; j < x; ++j) {
		if (row->buf[j] == '\t') {
			r += (TAB_SIZE - 1) - (r % TAB_SIZE);
		}
		r++;
	}
	return r;
}

size_t rx2cx(row_t *row, size_t r) {
	size_t cur_rx = 0;
	size_t my_cx;

	for (my_cx = 0; my_cx < row->size; ++my_cx) {
		if (row->buf[my_cx] == '\t') {
			cur_rx += (TAB_SIZE - 1) - (cur_rx % TAB_SIZE);
		}
		cur_rx++;
		if (cur_rx > r) {
			return my_cx;
		}
	}
	return my_cx;
}

void scroll() {
	rx = cx;
	if (cy < nrows) {
		rx = cx2rx(&rows[cy], cx);
	}

	if (cy < rowoff) {
		rowoff = cy;
	}
	if (cy >= rowoff + screenrows) {
		rowoff = cy - screenrows + 1;
	}
	if (rx < coloff) {
		coloff = rx;
	}
	if (rx >= coloff + screencols) {
		coloff = rx - screencols + 1;
	}
}

void draw_rows(mstring_t *ms) {
	int y;
	for (y = 0; y < screenrows; ++y) {
		size_t crow = y + rowoff;
		if (crow >= nrows)
			mstr_append(ms, "\x1b[34m~", 6);
		else {
			int len = rows[crow].rsize - coloff;
			if (len < 0) len = 0;
			if (len > screencols) len = screencols;
			mstr_append(ms, "\x1b[m", 3);
			mstr_append(ms, &rows[crow].render[coloff], len);
		}

		mstr_append(ms, "\x1b[K", 3);
		//if (y < screenrows - 1)
		mstr_append(ms, "\r\n", 2);
	}
	mstr_append(ms, "\x1b[m", 3);
}

void set_statusmsg(const char *format, ...) {
	va_list val;
	va_start(val, format);
	vsnprintf(statusmsg, screencols, format, val);
	va_end(val);
}

void draw_status(mstring_t *ms) {
	mstr_append(ms, "\x1b[7m", 4);

	double perc = 100 * (double)cy/(double)nrows;
	if (nrows == 0) perc = 0;

	char *status = (char*)malloc(screencols);
	size_t len = snprintf(status, screencols, "\"%s\"%s %dL %d,%d (%.1f%%)%s%s",
		curfile ? curfile : "[no name]",
		modified ? "*" : "",
		nrows, cy + 1, cx + 1, perc,
		strlen(statusmsg) > 0 ? " | " : "",
		strlen(statusmsg) > 0 ? statusmsg : "");
	if (len > screencols) len = screencols;

	mstr_append(ms, status, len);
	while (len < screencols) {
		mstr_append(ms, " ", 1);
		len++;
	}
	mstr_append(ms, "\x1b[m", 3);

}

void titlebar(mstring_t *ms) {
	char title[254];
	char path[254];
	if (curfile) GetFullPathNameA(curfile, sizeof(path), path, NULL);
	snprintf(title, sizeof(title), "%s%s%s%s%s",
		curfile ? curfile : "[no name]", modified ? "*" : "", curfile ? " (" : "", curfile ? path : "", curfile ? ")" : "");
	mstr_append(ms, "\x1b]0;", 4);
	mstr_append(ms, title, strlen(title));
	mstr_append(ms, "\0", 1);
}

void refresh() {
	scroll();
	
	mstring_t buf = MSTRING_INIT;
	titlebar(&buf);
	mstr_append(&buf, "\x1b[?25l", 6);
	mstr_append(&buf, "\x1b[H", 3);
	
	draw_rows(&buf);
	draw_status(&buf);

	char a[32];
	snprintf(a, sizeof(a), "\x1b[%d;%dH", (cy - rowoff) + 1, (rx - coloff) + 1);
	mstr_append(&buf, a, strlen(a));

	mstr_append(&buf, "\x1b[?25h", 6);
	WriteConsole(con1, buf.buf, buf.len, &dwwritten, NULL);
	mstr_free(&buf);
}

void update_row(row_t *row) {
	size_t tabs = 0;
	int j;
	
	for (j = 0; j < row->size; ++j) {
		if (row->buf[j] == '\t') tabs++;
	}

	free(row->render);
	row->render = (char*)malloc(row->size + tabs * (TAB_SIZE - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; ++j) {
		if (row->buf[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % TAB_SIZE != 0) row->render[idx++] = ' ';
		} else row->render[idx++] = row->buf[j];
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void insert_row(int at, char *s, size_t len) {
	if (at < 0 || at > nrows) return;

	rows = (row_t*)realloc(rows, (nrows + 1) * sizeof(row_t));
	memmove(&rows[at + 1], &rows[at], (nrows - at) * sizeof(row_t));

	rows[at].size = len;
	rows[at].buf = (char*)malloc(len + 1);
	memcpy(rows[at].buf, s, len);
	rows[at].buf[len] = '\0';

	rows[at].rsize = 0;
	rows[at].render = NULL;
	update_row(&rows[at]);
	nrows++;
	modified = 1;
}

void insert_newline() {
	if (cx == 0) {
		insert_row(cy, "", 0);
	} else {
		row_t *row = &rows[cy];
		insert_row(cy + 1, &row->buf[cx], row->size - cx);
		row = &rows[cy];
		row->size = cx;
		row->buf[row->size] = '\0';
		update_row(row);
	}
	cy++;
	cx = 0;
}

void row_free(row_t *row) {
	free(row->render);
	free(row->buf);
}

void row_del(int at) {
	if (at < 0 || at >= nrows) return;
	row_free(&rows[at]);
	memmove(&rows[at], &rows[at + 1], sizeof(row_t) * (nrows - at - 1));
	nrows--;
	modified = 1;
}

void row_append_string(row_t *row, char *s, size_t len) {
	row->buf = (char*)realloc(row->buf, row->size + len + 1);
	memcpy(&row->buf[row->size], s, len);
	row->size += len;
	row->buf[row->size] = '\0';
	update_row(row);
	modified = 1;
}

void row_insert_char(row_t *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	row->buf = (char*)realloc(row->buf, row->size + 2);
	memmove(&row->buf[at + 1], &row->buf[at], row->size - at + 1);
	row->size++;
	row->buf[at] = c;
	update_row(row);
	modified = 1;
}

void row_del_char(row_t *row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->buf[at], &row->buf[at + 1], row->size - at);
	row->size--;
	update_row(row);
	modified = 1;
}

void insert_char(int c) {
	if (cy == nrows) {
		insert_row(nrows, "", 0);
	}
	row_insert_char(&rows[cy], cx, c);
	cx++;
}

void del_char() {
	if (cy == nrows) return;
	if (cx == 0 && cy == 0) return;

	row_t *row = &rows[cy];
	if (cx > 0) {
		row_del_char(row, cx - 1);
		cx--;
	} else {
		cx = rows[cy - 1].size;
		row_append_string(&rows[cy - 1], row->buf, row->size);
		row_del(cy);
		cy--;
	}
}

char *my_getline(ssize_t *size, FILE *stream) {
    int c, growby = 80;
    ssize_t i = 0, linebufsize = 0;
    char *linebuf = NULL;

    while (1) {
        c = fgetc(stream);
        if (c == EOF) break;
        while (i > linebufsize - 2)
            linebuf = (char*)realloc(linebuf, linebufsize += growby);
        linebuf[i++] = (char)c;
        if (c == '\n' || c == '\0') break;
        
    }
    if (i == 0) return NULL;
    linebuf[i] = 0;
    *size = i;
    return linebuf;
}

void open_file(char *filename) {
	curfile = strdup(filename);
	FILE *fp = fopen(filename, "r");
	if (!fp) return;
	char *line;
	size_t linelen;
	while ((line = my_getline(&linelen, fp)) != NULL) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            line[linelen - 1] = '\0';
            linelen--;
        }
        insert_row(nrows, line, linelen);
    }
	free(line);
	fclose(fp);
	modified = 0;
}

char *rows2string(size_t *buflen) {
	size_t totlen = 0;
	int j;
	for (j = 0; j < nrows; ++j) {
		totlen += rows[j].size + 1;
	}
	*buflen = totlen;

	char *buf = (char*)malloc(totlen);
	char *p = buf;
	for (j = 0; j < nrows; ++j) {
		memcpy(p, rows[j].buf, rows[j].size);
		p += rows[j].size;
		*p = '\n';
		p++;
	}
	return buf;
}

int read_key() {
	INPUT_RECORD ir;
	KEY_EVENT_RECORD ker;
	while (1) {
		if (!ReadConsoleInputA(hstdin, &ir, 1, &dwread)) exit(1);
		if (ir.EventType != KEY_EVENT) continue;
		ker = ir.Event.KeyEvent;
		if (!ker.bKeyDown) continue;
		return ker.uChar.AsciiChar;
	}
}

char *prompt(char *s) {
	size_t size = 128;
	char *buf = (char*)malloc(size);
	size_t len = 0;
	buf[0] = '\0';

	while (1) {
		set_statusmsg(s, buf);
		refresh();

		int c = read_key();
		if (c == '\b' || c == '\x7f') {
			if (len != 0) {
				buf[--len] = '\0';
			}
		} else if (c == '\x1b') {
			set_statusmsg("");
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (len != 0) {
				set_statusmsg("");
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (len == size - 1) {
				size *= 2;
				buf = (char*)realloc(buf, size);
			}
			buf[len++] = c;
			buf[len] = '\0';
		}
	}
}

void find() {
	char *query = prompt("Search: %s");
	if (query == NULL) return;

	int i;
	for (i = 0; i < nrows; ++i) {
		row_t *row = &rows[i];
		char *match = strstr(row->render, query);
		if (match) {
			cy = i;
			cx = rx2cx(row, match - row->render);
			rowoff = nrows;
			break;
		}
	}
	free(query);
}

void save_file() {
	if (curfile == NULL) {
		curfile = prompt("Save as: %s");
		if (curfile == NULL) {
			set_statusmsg("Save aborted");
			return;
		}
	}

	size_t len;
	char *buf = rows2string(&len);
	FILE *fp = fopen(curfile, "wb");
	if (!fp) return;
    int fd = _fileno(fp);
    if (_chsize(fd, len) != -1) {
    	if (_write(fd, buf, len) == len) {
    		close(fd);
    		fclose(fp);
    		free(buf);
			modified = 0;
			set_statusmsg("%dB written to disk", len);
			return;
		}
	}
	close(fd);
	fclose(fp);
	free(buf);
	set_statusmsg("Write failed");
}

void read_key_event() {
	INPUT_RECORD ir;
	KEY_EVENT_RECORD ker;
	if (!ReadConsoleInputA(hstdin, &ir, 1, &dwread)) exit(1);
	if (ir.EventType != KEY_EVENT) return;
	ker = ir.Event.KeyEvent;
	if (!ker.bKeyDown) return;
	if (!iscntrl(ker.uChar.AsciiChar)) set_statusmsg(""); /* reset status message when a key is pressed */
	int lctrl_pressed = (ker.dwControlKeyState & LEFT_CTRL_PRESSED);
	int shift_pressed = (ker.dwControlKeyState & SHIFT_PRESSED);
	int lalt_pressed = (ker.dwControlKeyState & LEFT_ALT_PRESSED);
	row_t *row = (cy >= nrows) ? NULL : &rows[cy];
	int j;
	switch (ker.uChar.AsciiChar) {
		case CTRL_KEY('q'):
			quitting = 1;
			if (modified) {
				set_statusmsg("Quit without saving? (^Y = yes, ^N = no)");
				break;
			} else if (!modified) {
				SetConsoleMode(hstdin, hstdout_mode);
				exit(0);
				break;
			}
			break;
		case CTRL_KEY('y'):
			if (!quitting) break;
			SetConsoleMode(hstdin, hstdout_mode);
			exit(0);
			break;
		case CTRL_KEY('n'):
			if (!quitting) break;
			set_statusmsg("Quit aborted");
			quitting = 0;
			break;
		case CTRL_KEY('s'):
			save_file();
			break;
		case CTRL_KEY('f'):
			find();
			break;
		case '\b':
			del_char();
			break;
		case '\r':
			insert_newline();
			break;
		case 0:
			switch (ker.wVirtualKeyCode) {
				case VK_LEFT:
					if (cx != 0) cx--;
					else if (cy > 0 && lctrl_pressed) {
						cy--;
						cx = rows[cy].size;
					}
					break;
				case VK_RIGHT:
					if (row && cx < row->size) cx++;
					else if (row && cx == row->size && lctrl_pressed) {
						cy++;
						cx = 0;
					}
					break;
				case VK_UP:
					if (cy != 0) cy--;
					break;
				case VK_DOWN:
					if (cy < nrows) cy++;
					break;
				case VK_PRIOR:
					cy = rowoff;
					for (j = 0; j < screenrows; ++j) {
						if (cy != 0) cy--;
					}
					break;
				case VK_NEXT:
					cy = rowoff + screenrows - 1;
					if (cy > nrows) cy = nrows;
					for (j = 0; j < screenrows; ++j) {
						if (cy < nrows) cy++;
					}
					break;
				case VK_HOME:
					cx = 0;
					break;
				case VK_END:
					if (cy < nrows) {
						cx = rows[cy].size;
					}
					break;
				case VK_RETURN:
					insert_newline();
					//set_statusmsg("you pressed enter :>");
					break;
				case VK_DELETE:
					if (row && cx < row->size) cx++;
					del_char();
					break;
				case VK_INSERT:
					break;
				case VK_BACK:
					del_char();
					break;
				default:
					break;
			}
			row = (cy >= nrows) ? NULL : &rows[cy];
			size_t rowlen = row ? row->size : 0;
			if (cx > rowlen) cx = rowlen;
			break;
		default:
			insert_char(ker.uChar.AsciiChar);
			break;
	}
}

void ctrl_handler() {}

int main(int argc, char **argv) {
	hstdin = GetStdHandle(STD_INPUT_HANDLE);
	hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
	con1 = CreateConsoleScreenBuffer(
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			CONSOLE_TEXTMODE_BUFFER, NULL);
	con2 = CreateConsoleScreenBuffer(
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			CONSOLE_TEXTMODE_BUFFER, NULL);
	SetConsoleActiveScreenBuffer(con1);

	//SetConsoleMode(hstdin, GetConsoleMode(hstdin, &old_mode) | ENABLE_VIRTUAL_TERMINAL_INPUT);
	GetConsoleMode(hstdout, &hstdout_mode);


	GetConsoleMode(con1, &old_mode);
	mode = old_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
	mode |= DISABLE_NEWLINE_AUTO_RETURN | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	SetConsoleMode(con1, mode);
	SetConsoleMode(con2, mode);

	SetConsoleCtrlHandler(ctrl_handler, TRUE);

	SetConsoleCursorPosition(con1, (COORD){0, 0});

	GetConsoleScreenBufferInfo(con1, &csbi);
	GetConsoleCursorInfo(con1, &cci);

	screenrows = csbi.dwSize.Y;
	screencols = csbi.dwSize.X;
	cx = csbi.dwCursorPosition.X;
	cy = csbi.dwCursorPosition.Y;

	screenrows -= 1;

	statusmsg = (char*)malloc(screencols);
	statusmsg[0] = '\0';

	set_statusmsg("");

	if (argc >= 2)
		open_file(argv[1]);

	while (1) {
		refresh();
		read_key_event();
	}
}