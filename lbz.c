/* This file implements the Lua binding to libbzip2.
 *
 * Copyright (c) 2008, Evan Klitzke <evan@eklitzke.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <bzlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>

#include <assert.h>

#define LBZ_STATE_META "LuaBook.bz2"

typedef struct {
	BZFILE *bz_stream;
	FILE *f;
	char mode;

	/* getline related stuff */
	char *buf;
	size_t buf_size; /* max == LUAL_BUFFERSIZE */
} lbz_state;

/* Forward declarations */
static lbz_state *lbz_check_state(lua_State *L, int index);

static int lbz_open(lua_State *L);
static int lbz_read(lua_State *L);
static int lbz_write(lua_State *L);
static int lbz_perform_close(lua_State *L, lbz_state *state, int keep_extra_buf);
static int lbz_close(lua_State *L);
static int lbz_getline(lua_State *L);
static int lbz_getline_read(lua_State *L, luaL_Buffer *b, lbz_state *state, int keep_eol);

static int lbz_compress(lua_State *L);
static int lbz_decompress(lua_State *L);

static int lbz_lines(lua_State *L);

static void lbz_buffer_init(lbz_state *state);
static void lbz_buffer_free(lbz_state *state);

static void lbz_buffer_append(lbz_state *state, const char *data, size_t data_len);
static void lbz_buffer_drain(lbz_state *state, size_t amount);
static void lbz_buffer_drain_all(lbz_state *state);

static const struct luaL_reg bzlib_f [] = {
	{"open", lbz_open},
	{"compress", lbz_compress},
	{"decompress", lbz_decompress},
	{NULL, NULL} /* Sentinel */
};

static const struct luaL_reg bzlib_m [] = {
	{"read", lbz_read},
	{"write", lbz_write},
	{"getline", lbz_getline},
	{"close", lbz_close},
	{"lines", lbz_lines},
	{NULL, NULL} /* Sentinel */
};

lbz_state *lbz_check_state(lua_State *L, int index) {
	return (lbz_state *)luaL_checkudata(L, index, LBZ_STATE_META);
}

/* Binding to libbzip2's BZ2_bzReadOpen and BZ2_bzWriteOpen method */
int lbz_open(lua_State *L) {
	size_t len;
	const char *fname = lua_tolstring(L, 1, &len);
	size_t modelen;
	const char *mode = luaL_optlstring(L, 2, "r", &modelen);
	FILE *f = NULL;
	int level = luaL_optint(L, 3, 9);

	if (mode[0] == 'r') {
		f = fopen(fname, "rb");
	} else if (mode[0] == 'w') {
		f = fopen(fname, "wb");
	} else {
		return luaL_error(L, "Illegal mode: %s", lua_tostring(L, 1));
	}

	if (f == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, "Failed to open: ");
		lua_pushvalue(L, 1);
		lua_concat(L, 2);
		return 2;
	}

	int bzerror;
	lbz_state *state = (lbz_state *) lua_newuserdata(L, sizeof(lbz_state));

	state->f = f;
	state->mode = mode[0];

	if (mode[0] == 'r') {
		state->bz_stream = BZ2_bzReadOpen(&bzerror, f, 0, 0, NULL, 0);
		lbz_buffer_init(state);
	} else {
		state->bz_stream = BZ2_bzWriteOpen(&bzerror, f, level, 0, 0);
	}

	luaL_getmetatable(L, LBZ_STATE_META);
	lua_setmetatable(L, -2);

	if (bzerror != BZ_OK) {
		lua_pushnil(L);
		lua_pushstring(L, BZ2_bzerror(state->bz_stream, &bzerror));
		return 2;
	}

	return 1;
}

void lbz_buffer_init(lbz_state *state) {
	state->buf = malloc(LUAL_BUFFERSIZE);
	state->buf_size = 0;
}

void lbz_buffer_free(lbz_state *state) {
	if(!state->buf) return;
	state->buf_size = 0;
	free(state->buf);
	state->buf = NULL;
}

void lbz_buffer_append(lbz_state *state, const char *data, size_t data_size) {
	assert(state->buf_size + data_size < LUAL_BUFFERSIZE);
	memmove(state->buf + state->buf_size, data, data_size);
	state->buf_size += data_size;
}

void lbz_buffer_drain(lbz_state *state, size_t amount) {
	memmove(state->buf, state->buf + amount, state->buf_size - amount);
	state->buf_size -= amount;
}

void lbz_buffer_drain_all(lbz_state *state) {
	state->buf_size = 0;
}

/* Binding to libbzip2's BZ2_bzReadOpen method */
static int lbz_read(lua_State *L) {
	int bzerror = BZ_OK;
	int len = 0;
	luaL_Buffer b;
	lbz_state *state = lbz_check_state(L, 1);

	if (lua_type(L, 2) == LUA_TSTRING) {
		if (strcmp(lua_tostring(L, 2), "*a") == 0) {
			len = INT_MAX;
		} else {
			luaL_argerror(L, 2, "expecting number or '*a'");
		}
	} else {
		len = luaL_checkint(L, 2);
	}

	if (state->mode != 'r') {
		lua_pushnil(L);
		lua_pushstring(L, "NOT IN READ MODE");
		return 2;
	}

	if (!state->bz_stream && !state->buf) {
		/* The logical end of file has been reached -- there's no more data to
		 * return, and the user should call the close method. */
		lua_pushnil(L);
		lua_pushstring(L, "CLOSED");
		return 2;
	}
	luaL_buffinit(L, &b);

	/* In case this function is being used alongsize the getline method, we
	 * should use the buffers that getline is using */
	if (state->buf_size) {
		int used_len = (state->buf_size < len) ? state->buf_size : len;
		luaL_addlstring(&b, state->buf, used_len);
		lbz_buffer_drain(state, used_len);
		len -= used_len;
	}

	/* Pull in chunks until all data read */
	while(len > 0) {
		char *buf = luaL_prepbuffer(&b);
		int nextRead = len > LUAL_BUFFERSIZE ? LUAL_BUFFERSIZE : len;
		int read = BZ2_bzRead(&bzerror, state->bz_stream, buf, nextRead);
		if (read > 0) {
			luaL_addsize(&b, read);
			len -= read;
		}
		if (bzerror != BZ_OK)
			goto handle_error;
	}
	luaL_pushresult(&b);
	return 1;
handle_error:
	if(BZ_STREAM_END == bzerror) {
		/* Push the data read already and mark the stream done */
		luaL_pushresult(&b);
		lbz_perform_close(L, state, 0);
		return 1;
	} else {
		lua_pushnil(L);
		lua_pushstring(L, BZ2_bzerror(state->bz_stream, &bzerror));
		return 2;
	}
}

static int lbz_write(lua_State *L) {
	int bzerror = BZ_OK;
	lbz_state *state = lbz_check_state(L, 1);
	int i;

	if (state->mode != 'w') {
		lua_pushnil(L);
		lua_pushstring(L, "NOT IN WRITE MODE");
		return 2;
	}

	for (i=2; i<=lua_gettop(L); i++) {
		size_t len;
		const char *str = luaL_checklstring(L, i, &len);
		BZ2_bzWrite(&bzerror, state->bz_stream, (void*)str, len);
		if (bzerror != BZ_OK) {
			lua_pushnil(L);
			lua_pushstring(L, BZ2_bzerror(state->bz_stream, &bzerror));
			return 2;
		}
	}

	lua_pushboolean(L, 1);
	return 1;
}

int lbz_perform_close(lua_State *L, lbz_state *state, int keep_extra_buf) {
	int bzerror;
	int ret = 0;
	if (state->mode == 'r' && !keep_extra_buf) {
		lbz_buffer_free(state);
	}

	if(!state->bz_stream)
		return 0;

	if (state->mode == 'r') {
		BZ2_bzReadClose(&bzerror, state->bz_stream);
	} else {
		unsigned int in, out;
		BZ2_bzWriteClose(&bzerror, state->bz_stream, 0, &in, &out);
		lua_pushnumber(L, in);
		lua_pushnumber(L, out);
		ret = 2;
	}
	fclose(state->f);
	state->bz_stream = NULL;
	state->f = NULL;
	return ret;
}

/* Binding to libbzip2's BZ2_bzReadClose method */
static int lbz_close(lua_State *L) {
	lbz_state *state = lbz_check_state(L, 1);
	return lbz_perform_close(L, state, 0);
}

/*
 * GETLINE STUFF
 * This code is considerably more complicated... if you know of a simpler way
 * to do it that doesn't sacrifice speed, please let me know.
 */

static int lbz_handle_eol(luaL_Buffer *b, char *buf, size_t buf_len, lbz_state *state, int in_buffer, int keep_eol) {
	char *eol = memchr(buf, '\n', buf_len);
	size_t chars_to_return;

	/* If a newline hasn't been found, keep iterating while building up
	 * the buffer */
	if(eol == NULL) {
		if(in_buffer)
			luaL_addsize(b, buf_len);
		else
			luaL_addlstring(b, buf, buf_len);
		return 0;
	}
	chars_to_return = eol - buf;
	eol++;
	if(keep_eol)
		chars_to_return++;
	if(in_buffer)
		luaL_addsize(b, chars_to_return);
	else
		luaL_addlstring(b, buf, chars_to_return);
	/* Save the remaining data end of data - position of beginning */
	lbz_buffer_append(state, eol, buf_len - (eol - buf));
	luaL_pushresult(b);
	return 1;
}
/* This is an auxilliary function that lbz_getline calls when it needs to
 * actually use the BZ2_bzRead method to read more data from the bzipped file.
 **/
static int lbz_getline_read(lua_State *L, luaL_Buffer *b, lbz_state *state, int keep_eol) {
	int bzerror;

	/* The entire 'extra_buf' buffer is needed */
	luaL_addlstring(b, state->buf, state->buf_size);
	lbz_buffer_drain_all(state);

	if (!state->bz_stream) { /* No more data left at all - return data is 'success' */
		lbz_perform_close(L, state, 0); // Completely close it out now
		luaL_pushresult(b);
		return 1;
	}
	while(1) {
		char *buf = luaL_prepbuffer(b);
		int len = BZ2_bzRead(&bzerror, state->bz_stream, buf, LUAL_BUFFERSIZE);

		if ((bzerror != BZ_OK) && (bzerror != BZ_STREAM_END)) {
			/* Error happened, data thrown */
			lua_pushnil(L);
			lua_pushstring(L, BZ2_bzerror(state->bz_stream, &bzerror));
			return 2;
		}
		if (!lbz_handle_eol(b, buf, len, state, 1, keep_eol))
			continue;

		/* Kill the stream, keep the remaining buffer */
		if (bzerror == BZ_STREAM_END)
			lbz_perform_close(L, state, state->buf_size ? 1 : 0);
		return 1;
	}
	return 0;
}

static int lbz_getline(lua_State *L) {
	lbz_state *state = lbz_check_state(L, 1);
	int skip_eol = lua_toboolean(L, 2);
	luaL_Buffer b;

	if (!state->bz_stream && !state->buf) {
		lua_pushnil(L);
		lua_pushstring(L, "CLOSED");
		return 2;
	}

	luaL_buffinit(L, &b);
	if (state->buf_size) {
		size_t data_size = state->buf_size;
		lbz_buffer_drain_all(state);
		/* Drain entire buffer so that remaining data can be appropriately added */
		if (!lbz_handle_eol(&b, state->buf, data_size, state, 0, !skip_eol))
			return lbz_getline_read(L, &b, state, !skip_eol);
		return 1;
	}

	/* If there was no extra data from the last pass then we need to call
	 * lbz_getline_read directly to get more data and find the newline. */
	return lbz_getline_read(L, &b, state, !skip_eol);
}

static int lbz_line_iter(lua_State *L) {
	lua_settop(L, 0);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, lua_upvalueindex(2));
	return lbz_getline(L);
}

/* (bz):lines(keep_eol) */
int lbz_lines(lua_State *L) {
	int skip_eol = !lua_toboolean(L, 2);
	lua_pushvalue(L, 1);
	lua_pushboolean(L, skip_eol);
	lua_pushcclosure(L, lbz_line_iter, 2);
	return 1;
}

static int lbz_gc(lua_State *L) {
	lbz_close(L);
	return 0;
}

/* low-level compression
	bz2.compress(s, [level]) -> string -- level defaults to 9
*/
static int lbz_compress(lua_State *L) {
	size_t len;
	const char *input = luaL_checklstring(L, 1, &len);
	int level = luaL_optinteger(L, 2, 9);
	int err;
	bz_stream stream = {0};

	err = BZ2_bzCompressInit(&stream, level, 0, 0);
	if (err != BZ_OK)
		goto handle_error;

	luaL_Buffer b;
    luaL_buffinit(L, &b);

	stream.next_in = (char*) input;
	stream.avail_in = len;

	/* feed input data */
	while (stream.total_in_lo32 < len) {
		stream.next_out = (char*) luaL_prepbuffer(&b);
		stream.avail_out = LUAL_BUFFERSIZE;

		err = BZ2_bzCompress(&stream, BZ_RUN);
		if (err < 0)
			goto handle_error;

		luaL_addsize(&b, LUAL_BUFFERSIZE - stream.avail_out);
	}

	/* save any lefover data */
	while (1) {
		stream.next_out = (char*) luaL_prepbuffer(&b);
		stream.avail_out = LUAL_BUFFERSIZE;

		err = BZ2_bzCompress(&stream, BZ_FINISH);
		if (err < 0)
			goto handle_error;

		luaL_addsize(&b, LUAL_BUFFERSIZE - stream.avail_out);
		if (err == BZ_STREAM_END)
			break;
	}

	BZ2_bzCompressEnd(&stream);

	luaL_pushresult(&b);
	return 1;

handle_error:
	lua_pushnil(L);
	lua_pushstring(L, BZ2_bzerror(&stream, &err));
	return 2;
}

/* low-level decompression
	bz2.decompress(s) -> string
*/
static int lbz_decompress(lua_State *L) {
	size_t len;
	const char *input = luaL_checklstring(L, 1, &len);
	int err;
	bz_stream stream = {0};

	err = BZ2_bzDecompressInit(&stream, 0, 0);
	if (err != BZ_OK)
		goto handle_error;

	luaL_Buffer b;
    luaL_buffinit(L, &b);

	stream.next_in = (char*) input;
	stream.avail_in = len;

	/* feed input data */
	while (stream.total_in_lo32 < len) {
		stream.next_out = (char*) luaL_prepbuffer(&b);
		stream.avail_out = LUAL_BUFFERSIZE;

		err = BZ2_bzDecompress(&stream);
		if (err < 0)
			goto handle_error;

		luaL_addsize(&b, LUAL_BUFFERSIZE - stream.avail_out);
	}

	BZ2_bzDecompressEnd(&stream);
	luaL_pushresult(&b);
	return 1;

handle_error:
	lua_pushnil(L);
	lua_pushstring(L, BZ2_bzerror(&stream, &err));
	return 2;
}


int luaopen_bz2(lua_State *L) {
	luaL_newmetatable(L, LBZ_STATE_META);
	lua_newtable(L);
	luaL_register(L, NULL, bzlib_m);
	lua_setfield(L, -2, "__index");

	lua_pushcfunction(L, lbz_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);

	luaL_register(L, "bz2", bzlib_f);
	return 1;
}
