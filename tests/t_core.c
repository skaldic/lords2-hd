/* t_core.c - tier 1: the patch's logic that needs no game memory. Our own code. Included by tests.c. */

/* ================================ mem_readable (the guard behind the 1.5.0 minimap crash fix) */

TEST(mem_readable_plain_memory)
{
	BYTE stack[64];
	BYTE *heap = malloc(100000);
	REQUIRE(heap);
	CHECK(mem_readable(stack, sizeof stack));
	CHECK(mem_readable(heap, 100000));
	CHECK(mem_readable(heap, 0));               // nothing to read
	CHECK(!mem_readable(NULL, 4));
	CHECK(!mem_readable(NULL, 0));
	CHECK(!mem_readable(heap, (size_t)-16));    // a range that wraps round the address space
	free(heap);
}

TEST(mem_readable_freed_and_reserved)
{
	BYTE *p = VirtualAlloc(NULL, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	REQUIRE(p);
	CHECK(mem_readable(p, 2 * 4096));
	VirtualFree(p, 0, MEM_RELEASE);
	CHECK(!mem_readable(p, 1));                 // freed: what the game does to the minimap buffers after a battle
	BYTE *r = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_READWRITE);
	REQUIRE(r);
	CHECK(!mem_readable(r, 1));                 // reserved, never committed
	VirtualFree(r, 0, MEM_RELEASE);
}

TEST(mem_readable_protection)
{
	BYTE *p = VirtualAlloc(NULL, 3 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	REQUIRE(p);
	DWORD old;
	VirtualProtect(p + 4096, 4096, PAGE_NOACCESS, &old);
	VirtualProtect(p + 2 * 4096, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
	CHECK(mem_readable(p, 4096));               // the first page alone
	CHECK(mem_readable(p + 4000, 96));          // up to the end of the first page
	CHECK(!mem_readable(p + 4000, 97));         // one byte into the no-access page
	CHECK(!mem_readable(p + 4096, 1));
	CHECK(!mem_readable(p + 2 * 4096, 1));      // a guard page (asking must not trip it)
	CHECK(!mem_readable(p, 3 * 4096));
	VirtualProtect(p + 4096, 4096, PAGE_READONLY, &old);
	CHECK(mem_readable(p, 2 * 4096));           // read-only counts as readable
	VirtualFree(p, 0, MEM_RELEASE);
}

/* ================================ the game folder's name (game_dir: a player's black F10 scroll and no HD art) */

static BOOL t_cjk_codepage(void) { UINT cp = GetACP(); return cp == 932 || cp == 936 || cp == 949 || cp == 950; }

TEST(ansi_exact_only_without_loss)
{
	char a[MAX_PATH];
	CHECK(ansi_exact(L"C:\\Games\\Lords of the Realm II", a, sizeof a));
	CHECK(!strcmp(a, "C:\\Games\\Lords of the Realm II"));
	if (!t_cjk_codepage()) CHECK(!ansi_exact(L"C:\\Games\\\x4E09", a, sizeof a));   // no byte for it: "?"
	// best fit ("\x141ukasz" as "Lukasz") names another folder: refused unless the code page has the letter
	WCHAR back[MAX_PATH];
	if (ansi_exact(L"C:\\\x141ukasz", a, sizeof a)) {
		MultiByteToWideChar(CP_ACP, 0, a, -1, back, MAX_PATH);
		CHECK(!wcscmp(back, L"C:\\\x141ukasz"));
	}
	CHECK(!ansi_exact(L"C:\\Games", a, 4));    // too long for the buffer
}

TEST(game_dir_opens_files_in_any_folder)
{
	WCHAR root[MAX_PATH], plain[MAX_PATH], odd[MAX_PATH], file[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, g_gameDir, -1, root, MAX_PATH);
	char dir[MAX_PATH], p[MAX_PATH];
	// a plain name: itself
	swprintf(plain, MAX_PATH, L"%ls\\Lords of the Realm II", root);
	CreateDirectoryW(plain, NULL);
	CHECK(game_dir_of(plain, dir, sizeof dir) == NULL);
	snprintf(p, sizeof p, "%s\\Lords of the Realm II", g_gameDir);
	CHECK(!strcmp(dir, p));
	// a name no code page byte can spell (Cyrillic and Chinese; on an East Asian Windows only the Cyrillic may fail)
	swprintf(odd, MAX_PATH, L"%ls\\\x41B\x43E\x440\x434\x44B \x4E09", root);
	REQUIRE(CreateDirectoryW(odd, NULL));
	swprintf(file, MAX_PATH, L"%ls\\Base01.256", odd);
	FILE *w = _wfopen(file, L"wb");
	REQUIRE(w);
	fputs("palette", w);
	fclose(w);
	const char *how = game_dir_of(odd, dir, sizeof dir);
	if (!how) return;                          // (this Windows spells it exactly)
	CHECK(!strchr(dir, '?'));
	if (!strcmp(how, "its short name")) {      // the 8.3 name: the patch's narrow fopen finds the file
		snprintf(p, sizeof p, "%s\\Base01.256", dir);
		FILE *f = fopen(p, "rb");
		CHECK(f != NULL);
		if (f) fclose(f);
	}
	else CHECK(!strcmp(dir, "."));             // no 8.3 names on this drive: the current folder, the game's own
}

TEST(settings_scroll_without_map_palette_is_not_black)
{
	// No Base01.256 in this test's game folder: map_lut fails (once for the run; no test before this reads it)
	SCROLL_OVL *s = &g_ovl[1];
	memset(g_ovl, 0, sizeof g_ovl);
	enum { X = 10, Y = 20, W = 8, H = 4, SW = 640 };
	static BYTE snap[W * H];
	for (int r = 0; r < H; r++)
		for (int c = 0; c < W; c++) g_frame8[(Y + r) * SW + X + c] = snap[r * W + c] = (BYTE)(1 + r * W + c);
	for (int r = 0; r < 2 * H; r++)
		for (int c = 0; c < 2 * W; c++) g_frame2x[(2 * Y + r) * (2 * SW) + 2 * X + c] = 0x00100000u + (DWORD)(r * 64 + c);
	int off = g_off; g_off = 0;
	s->x = X; s->y = Y; s->w = W; s->h = H; s->snap = snap; s->under = NULL; s->mapPal = 1; s->gen = 1; s->on = 1;
	scroll_overlays_catch(0, SW);
	CHECK(g_ovlShow[1]);
	REQUIRE(s->cache);
	int black = 0, wrong = 0;
	for (int r = 0; r < 2 * H; r++)
		for (int c = 0; c < 2 * W; c++) {
			DWORD v = s->cache[r * 2 * W + c];
			if (!v) black++;
			if (v != 0x00100000u + (DWORD)(r * 64 + c)) wrong++;
		}
	CHECK_EQ(black, 0);                        // the empty cache used to be laid on: a black scroll
	CHECK_EQ(wrong, 0);                        // the game's own pixels instead
	for (int r = 0; r < 2 * H; r++) memset(g_frame2x + (2 * Y + r) * (2 * SW) + 2 * X, 0, 2 * W * sizeof(DWORD));
	scroll_overlays_lay(SW);
	CHECK_EQ(g_frame2x[(2 * Y) * (2 * SW) + 2 * X], 0x00100000u);
	g_off = off;
	free(s->cache);
	memset(g_ovl, 0, sizeof g_ovl);
	memset(g_frame8, 0, sizeof g_frame8);
	memset(g_frame2x, 0, sizeof g_frame2x);
}

/* ================================ the art pack (tools\l2pak.c writes it, pak_open / afopen read it) */

static BYTE *t_pattern(size_t n, unsigned seed)
{
	BYTE *b = malloc(n ? n : 1);
	t_rand_state = seed;
	for (size_t i = 0; i < n; i++) b[i] = (BYTE)t_rand();
	return b;
}
/* Packs <game>\src into <game>\lords2hd.pak with the real packer. */
static int t_pack(void)
{
	static const char *const need[] = { "screens", "wide", "tiles", "sprites" };   // the packer wants these four
	for (int i = 0; i < 4; i++) { char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\src\\%s\\", g_gameDir, need[i]); t_mkdirs(d); }
	char cmd[1024];
	snprintf(cmd, sizeof cmd, "\"%s\\l2pak.exe\" \"%s\\src\" \"%s\\lords2hd.pak\"", t_exeDir, g_gameDir, g_gameDir);
	return t_exec(cmd);
}
static BOOL t_read_all(const char *rel, BYTE **data, long *size)   // through afopen, as the patch reads art
{
	char p[MAX_PATH];
	t_path(p, sizeof p, rel);
	AFILE *f = afopen(p);
	if (!f) return FALSE;
	afseek(f, 0, SEEK_END);
	*size = aftell(f);
	afseek(f, 0, SEEK_SET);
	*data = malloc(*size ? *size : 1);
	BOOL ok = *data && afread(*data, 1, *size, f) == (size_t)*size;
	afclose(f);
	return ok;
}

TEST(pak_round_trip)
{
	static const struct { const char *src, *in_pak; size_t size; } files[] = {
		{ "src\\screens\\castle.bin", "lords2hd\\screens\\castle.bin", 70001 },
		{ "src\\wide\\MERCHANT.BIN", "lords2hd\\wide\\merchant.bin", 3000 },   // names go lower case
		{ "src\\tiles\\land.pal", "lords2hd\\tiles\\land.pal", 768 },
		{ "src\\sprites\\knight.bin", "lords2hd\\sprites\\knight.bin", 1 },
		{ "src\\sound\\mace_u1.wav", "lords2hd\\sound\\mace_u1.wav", 5000 },
		{ "src\\videohd\\intro.bin", "lords2hd\\videohd\\intro.bin", 1 << 20 },
	};
	int n = sizeof files / sizeof *files;
	for (int i = 0; i < n; i++) { BYTE *b = t_pattern(files[i].size, 100 + i); t_write(files[i].src, b, files[i].size); free(b); }
	t_write("src\\screens\\notes.txt", "not art", 7);      // not packed: only .bin/.pal there
	t_write("src\\sound\\voice.mp3", "not a wav", 9);      // not packed: only .wav there
	REQUIRE(t_pack() == 0);
	pak_open();
	CHECK_EQ(g_pakN, n);
	for (int i = 0; i < n; i++) {
		BYTE *got = NULL; long size = 0;
		BYTE *want = t_pattern(files[i].size, 100 + i);
		BOOL ok = t_read_all(files[i].in_pak, &got, &size);
		CHECK(ok);
		if (ok) { CHECK_EQ(size, (long)files[i].size); CHECK(size == (long)files[i].size && !memcmp(got, want, size)); }
		free(got); free(want);
	}
	char p[MAX_PATH];
	t_path(p, sizeof p, "lords2hd\\SCREENS\\Castle.BIN");
	CHECK(aexists(p));                          // any case, as Windows file names
	t_path(p, sizeof p, "lords2hd\\screens\\notes.txt");
	CHECK(!aexists(p));
	t_path(p, sizeof p, "lords2hd\\screens\\missing.bin");
	CHECK(!aexists(p));
	CHECK(afopen(p) == NULL);
	t_path(p, sizeof p, "elsewhere\\castle.bin");   // only paths under <game>\lords2hd\ are in the pack
	CHECK(!aexists(p));
}

TEST(pak_loose_file_wins)
{
	BYTE *b = t_pattern(4000, 7);
	t_write("src\\screens\\castle.bin", b, 4000);
	REQUIRE(t_pack() == 0);
	pak_open();
	t_write("lords2hd\\screens\\castle.bin", "loose", 5);  // a file replaced by hand
	BYTE *got = NULL; long size = 0;
	CHECK(t_read_all("lords2hd\\screens\\castle.bin", &got, &size));
	CHECK_EQ(size, 5);
	CHECK(size == 5 && !memcmp(got, "loose", 5));
	free(got); free(b);
}

TEST(pak_reads_past_the_end_and_seeks)
{
	BYTE *b = t_pattern(1001, 9);
	t_write("src\\tiles\\t.bin", b, 1001);
	REQUIRE(t_pack() == 0);
	pak_open();
	char p[MAX_PATH];
	t_path(p, sizeof p, "lords2hd\\tiles\\t.bin");
	AFILE *f = afopen(p);
	REQUIRE(f);
	REQUIRE(f->f == NULL);                      // from the pack, not a loose file
	BYTE buf[2048];
	CHECK_EQ(afseek(f, 1000, SEEK_SET), 0);
	CHECK_EQ(afread(buf, 1, 100, f), 1);        // only the last byte is left
	CHECK_EQ(buf[0], b[1000]);
	CHECK_EQ(afread(buf, 1, 100, f), 0);
	CHECK_EQ(afseek(f, 990, SEEK_SET), 0);
	CHECK_EQ(afread(buf, 4, 10, f), 2);         // 11 bytes left: two whole 4-byte items
	CHECK(!memcmp(buf, b + 990, 8));
	CHECK_EQ(afseek(f, -1, SEEK_END), 0);
	CHECK_EQ(aftell(f), 1000);
	CHECK_EQ(afseek(f, 1, SEEK_END), -1);       // past the end
	CHECK_EQ(afseek(f, -2000, SEEK_CUR), -1);   // before the start
	CHECK_EQ(aftell(f), 1000);                  // (unchanged by a refused seek)
	afclose(f);
	free(b);
}

static void t_write_pak_head(DWORD magic, DWORD count, DWORD idx, DWORD version, size_t pad)
{
	DWORD head[4] = { magic, count, idx, version };
	BYTE *b = calloc(1, sizeof head + pad);
	memcpy(b, head, sizeof head);
	t_write("lords2hd.pak", b, sizeof head + pad);
	free(b);
}

TEST(pak_rejects_bad_files)
{
	t_write_pak_head(PAK_MAGIC, 1, 16, 1, 64);           // version 1 (the old scrambled pack)
	pak_open(); CHECK(g_pak == NULL);
	t_write_pak_head(0x12345678, 1, 16, PAK_VERSION, 64);  // not a pack
	pak_open(); CHECK(g_pak == NULL);
	t_write_pak_head(PAK_MAGIC, 0, 16, PAK_VERSION, 64);   // empty
	pak_open(); CHECK(g_pak == NULL);
	t_write_pak_head(PAK_MAGIC, 200000, 16, PAK_VERSION, 64);  // a count no pack has
	pak_open(); CHECK(g_pak == NULL);
	t_write_pak_head(PAK_MAGIC, 3, 16, PAK_VERSION, sizeof(PAK_ENTRY) * 2);  // index cut short
	pak_open(); CHECK(g_pak == NULL);
	t_write("lords2hd.pak", "C2", 2);                      // shorter than a header
	pak_open(); CHECK(g_pak == NULL);
}

TEST(pak_names_are_terminated)
{
	// an index entry whose name fills all 48 bytes must not run into the next field
	PAK_ENTRY e;
	memset(e.name, 'a', sizeof e.name);
	e.off = 16; e.size = 0;
	DWORD head[4] = { PAK_MAGIC, 1, 16, PAK_VERSION };
	BYTE b[16 + sizeof e];
	memcpy(b, head, 16); memcpy(b + 16, &e, sizeof e);
	t_write("lords2hd.pak", b, sizeof b);
	pak_open();
	REQUIRE(g_pak);
	CHECK_EQ(strlen(g_pak[0].name), sizeof e.name - 1);
}

/* ================================ scaling (Catmull-Rom, FSR 1 on the processor) */

TEST(catmull_rom_weights_sum_to_one)
{
	static const int sizes[][2] = { { 640, 1280 }, { 640, 1920 }, { 1280, 2560 }, { 1280, 3440 }, { 7, 13 }, { 640, 333 }, { 1920, 640 }, { 1, 5 } };
	for (int s = 0; s < (int)(sizeof sizes / sizeof *sizes); s++) {
		cr_axis a = { 0 };
		REQUIRE(cr_axis_make(&a, sizes[s][0], sizes[s][1], 1024));
		CHECK_EQ(a.n, sizes[s][1]);
		CHECK(a.taps % 2 == 0 && a.taps <= CR_MAXTAPS);
		int bad = 0;
		for (int o = 0; o < a.n; o++) {
			int sum = 0;
			for (int t = 0; t < a.taps; t++) {
				sum += a.w[o * a.taps + t];
				int i = a.idx[o * a.taps + t];
				if (i < 0 || i >= sizes[s][0]) bad++;
			}
			if (sum != 1024) bad++;
		}
		if (bad) printf("    %d -> %d: %d bad weights or indexes\n", sizes[s][0], sizes[s][1], bad);
		CHECK_EQ(bad, 0);
		free(a.idx); free(a.w);
	}
}

/* fills w x h of a buffer (stride in pixels) with one colour and the rest with a canary */
#define T_CANARY 0xDEADBEEFu
static DWORD *t_image(int w, int h, int stride, int rows, DWORD colour)
{
	DWORD *p = malloc((size_t)stride * rows * 4);
	for (int i = 0; i < stride * rows; i++) p[i] = T_CANARY;
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) p[y * stride + x] = colour;
	return p;
}
/* every pixel within tol of colour per channel; nothing outside w x h touched */
static void t_check_flat(const DWORD *p, int w, int h, int stride, int rows, DWORD colour, int tol, const char *what)
{
	int off = 0, touched = 0;
	for (int y = 0; y < rows; y++) for (int x = 0; x < stride; x++) {
		DWORD v = p[y * stride + x];
		if (x < w && y < h) {
			for (int c = 0; c < 24; c += 8) if (abs((int)((v >> c) & 255) - (int)((colour >> c) & 255)) > tol) { off++; break; }
		}
		else if (v != T_CANARY) touched++;
	}
	if (off || touched) printf("    %s: %d pixels off, %d outside the picture written\n", what, off, touched);
	CHECK_EQ(off, 0);
	CHECK_EQ(touched, 0);
}

TEST(catmull_rom_flat_stays_flat)
{
	static const int sizes[][4] = { { 640, 480, 1280, 960 }, { 640, 480, 1913, 1437 }, { 33, 17, 70, 37 }, { 1280, 960, 1000, 750 } };
	static const DWORD colours[] = { 0x00000000, 0x00FFFFFF, 0x00FF0000, 0x00123456 };
	for (int s = 0; s < 4; s++) for (int c = 0; c < 4; c++) {
		int sw = sizes[s][0], sh = sizes[s][1], dw = sizes[s][2], dh = sizes[s][3];
		DWORD *src = t_image(sw, sh, sw + 3, sh, colours[c]);
		DWORD *dst = t_image(0, 0, dw + 5, dh + 2, 0);
		REQUIRE(resize_cr(src, sw, sh, sw + 3, dst, dw, dh, dw + 5));
		char what[64]; snprintf(what, sizeof what, "%dx%d -> %dx%d colour %06lx", sw, sh, dw, dh, colours[c]);
		t_check_flat(dst, dw, dh, dw + 5, dh + 2, colours[c], 2, what);   // (2: the vertical pass's rounding)
		free(src); free(dst);
	}
	CHECK(!resize_cr(NULL, 0, 480, 640, NULL, 1280, 960, 1280));
}

TEST(fsr_flat_stays_flat)
{
	static const DWORD colours[] = { 0x00000000, 0x00FFFFFF, 0x0000FF00, 0x00808080 };
	for (int c = 0; c < 4; c++) {
		DWORD *src = t_image(320, 240, 323, 240, colours[c]);
		DWORD *dst = t_image(0, 0, 645, 482, 0);
		REQUIRE(fsr_easu(src, 320, 240, 323, dst, 640, 480, 645));
		char what[64]; snprintf(what, sizeof what, "EASU colour %06lx", colours[c]);
		t_check_flat(dst, 640, 480, 645, 482, colours[c], 1, what);
		DWORD *sharp = t_image(0, 0, 650, 481, 0);
		REQUIRE(fsr_rcas(dst, 645, sharp, 640, 480, 650));
		snprintf(what, sizeof what, "RCAS colour %06lx", colours[c]);
		t_check_flat(sharp, 640, 480, 650, 481, colours[c], 1, what);
		free(src); free(dst); free(sharp);
	}
	CHECK(!fsr_rcas(NULL, 8, NULL, 7, 10, 8));  // too narrow
}

TEST(colour_mixing)
{
	for (int i = 0; i < 1000; i++) {
		DWORD a = t_rand() & 0xffffff, b = t_rand() & 0xffffff;
		if (mix4(a, a, a, a) != a) { CHECK(mix4(a, a, a, a) == a); break; }
		if (mix_rgb(a, b, 0) != a || mix_rgb(a, b, 256) != b) { CHECK(mix_rgb(a, b, 0) == a && mix_rgb(a, b, 256) == b); break; }
	}
	CHECK_EQ(mix4(0xff0000, 0, 0, 0), 0x8f0000);    // 9/16 of the nearest pixel
	CHECK_EQ(mix4(0, 0, 0, 0x0000ff), 0x00000f);    // 1/16 of the farthest
	CHECK_EQ(mix4(0xffffff, 0xffffff, 0xffffff, 0xffffff), 0xffffff);   // no carry between channels
	CHECK_EQ(mix_rgb(0x000000, 0xffffff, 128), 0x7f7f7f);
}

/* ================================ HD video frames (the .bin beside each video, see hv_open) */

/* A JPEG of one colour (Windows' own encoder), w x h. */
static BYTE *t_jpeg(int w, int h, DWORD bgr, DWORD *size)
{
	BYTE *out = NULL;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	IWICImagingFactory *fac = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *fr = NULL; IPropertyBag2 *props = NULL; IStream *st = NULL;
	BYTE *px = malloc((size_t)w * h * 3);
	for (int i = 0; i < w * h; i++) { px[i * 3] = bgr & 255; px[i * 3 + 1] = (bgr >> 8) & 255; px[i * 3 + 2] = (bgr >> 16) & 255; }
	WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
	if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac)) &&
		SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &st)) &&
		SUCCEEDED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg, NULL, &enc)) &&
		SUCCEEDED(IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props)) &&
		SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props)) &&
		SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h)) &&
		SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)) &&
		SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 3, w * h * 3, px)) &&
		SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
		LARGE_INTEGER zero = { 0 }; ULARGE_INTEGER pos;
		HGLOBAL hg;
		if (SUCCEEDED(IStream_Seek(st, zero, STREAM_SEEK_CUR, &pos)) && SUCCEEDED(GetHGlobalFromStream(st, &hg))) {
			*size = (DWORD)pos.QuadPart;
			out = malloc(*size);
			memcpy(out, GlobalLock(hg), *size);
			GlobalUnlock(hg);
		}
	}
	if (props) IPropertyBag2_Release(props);
	if (fr) IWICBitmapFrameEncode_Release(fr);
	if (enc) IWICBitmapEncoder_Release(enc);
	if (st) IStream_Release(st);
	if (fac) IWICImagingFactory_Release(fac);
	free(px);
	return out;
}
/* <game>\lords2hd\videohd\<stem>.bin for a w x h video of n frames, frame i coloured colours[i % 2]. offs_cut: offsets
 * written short of n + 1 (a truncated file). */
static void t_hv_file(const char *stem, int w, int h, int n, int offs_cut)
{
	static const DWORD colours[2] = { 0x2040c0, 0xc08020 };
	DWORD jsz[2]; BYTE *j[2] = { t_jpeg(2 * w, 2 * h, colours[0], &jsz[0]), t_jpeg(2 * w, 2 * h, colours[1], &jsz[1]) };
	size_t cap = 32 + 4 * (n + 1) + (size_t)n * (jsz[0] + jsz[1]);
	BYTE *b = malloc(cap), *p = b;
	DWORD head[8] = { HV_MAGIC, 1, 2u * w, 2u * h, (DWORD)n, (DWORD)w, (DWORD)h, 0 };
	memcpy(p, head, 32); p += 32;
	DWORD *offs = (DWORD *)p, off = 32 + 4 * (n + 1);
	for (int i = 0; i <= n; i++) { offs[i] = off; if (i < n) off += jsz[i % 2]; }
	p += 4 * (n + 1);
	for (int i = 0; i < n; i++) { memcpy(p, j[i % 2], jsz[i % 2]); p += jsz[i % 2]; }
	size_t len = offs_cut ? 32 + 4 * (n + 1 - offs_cut) : (size_t)(p - b);
	char rel[MAX_PATH];
	snprintf(rel, sizeof rel, "lords2hd\\videohd\\%s.bin", stem);
	t_write(rel, b, len);
	free(b); free(j[0]); free(j[1]);
}

TEST(video_frames_open)
{
	static int smk;   // stands for the game's HSMACK
	t_hv_file("castle1", 16, 8, 3, 0);
	hv_open(&smk, "C:\\GOG\\Lords2\\Videos\\CASTLE1.SMK", 16, 8, 3);
	CHECK(g_hv.smk == &smk);
	CHECK(g_hv.f != NULL);
	CHECK_EQ(g_hv.n, 3);
	CHECK_EQ(g_hv.W, 32);
	CHECK_EQ(g_hv.H, 16);
	hv_open(&smk, "nothing.smk", 16, 8, 3);    // no frames for this one: the open video stays as it was
	CHECK(g_hv.smk == &smk);
}

TEST(video_frames_that_dont_fit)
{
	static int smk;
	t_hv_file("intro", 16, 8, 3, 0);
	hv_open(&smk, "intro.smk", 18, 8, 3);      // another width
	CHECK(g_hv.smk == NULL);
	hv_open(&smk, "intro.smk", 16, 8, 4);      // another frame count
	CHECK(g_hv.smk == NULL);
	g_hdVideos = FALSE;                         // HDVideos=0
	hv_open(&smk, "intro.smk", 16, 8, 3);
	CHECK(g_hv.smk == NULL);
}

TEST(video_frames_truncated_index)
{
	// 1.5.0 review fix: a short offset table must leave no video half open
	static int smk, other;
	t_hv_file("good", 16, 8, 3, 0);
	hv_open(&other, "good.smk", 16, 8, 3);
	REQUIRE(g_hv.smk == &other);
	t_hv_file("credits", 16, 8, 5, 2);
	hv_open(&smk, "credits.smk", 16, 8, 5);
	CHECK(g_hv.smk == NULL);
	CHECK(g_hv.f == NULL);
	CHECK(g_hv.offs == NULL);
}

TEST(video_frame_decode_and_swap)
{
	static BYTE smk[1024];   // the HSMACK fields the patch reads: FrameNum and the palette
	static BYTE dest[8 * 16];
	t_hv_file("battle1", 16, 8, 3, 0);
	hv_open(smk, "battle1.smk", 16, 8, 3);
	REQUIRE(g_hv.smk == smk);
	g_hv.dest = dest; g_hv.pitch = 16;
	*(int *)(smk + SMK_FRAMENUM) = 1;
	hv_frame(smk);
	CHECK_EQ(g_hv.on, 1);
	CHECK_EQ(g_hv.frame, 1);
	REQUIRE(g_hv.valid[g_hv.newest]);
	DWORD v = g_hv.px[g_hv.newest][5 * 32 + 7];  // frame 1 is 0xc08020
	CHECK_NEAR((v >> 16) & 255, 0xc0, 3);
	CHECK_NEAR((v >> 8) & 255, 0x80, 3);
	CHECK_NEAR(v & 255, 0x20, 3);
	*(int *)(smk + SMK_FRAMENUM) = 3;           // past the last frame: off for this video
	hv_frame(smk);
	CHECK_EQ(g_hv.on, 0);
	CHECK(g_hv.f == NULL);
}

TEST(video_jpeg_decoder)
{
	DWORD size = 0;
	BYTE *j = t_jpeg(24, 16, 0x336699, &size);
	REQUIRE(j && size > 100);
	DWORD *out = calloc(24 * 16, 4);
	CHECK(hv_decode(j, size, out, 24, 16));
	CHECK_NEAR((out[100] >> 16) & 255, 0x33, 3);
	CHECK_NEAR(out[100] & 255, 0x99, 3);
	CHECK(!hv_decode(j, size, out, 24, 17));    // not the size the video expects
	CHECK(!hv_decode(j, size / 3, out, 24, 16)); // cut short
	BYTE junk[300]; memset(junk, 0x5a, sizeof junk);
	CHECK(!hv_decode(junk, sizeof junk, out, 24, 16));
	free(out); free(j);
}

/* ================================ sound files */

/* a WAV: RIFF, optional extra chunk before fmt, fmt, data (frames of ch x bits) */
static BYTE *t_wav(int ch, int bits, DWORD rate, const void *data, DWORD bytes, BOOL extra, DWORD *len)
{
	BYTE *b = malloc(64 + bytes), *p = b + 12;
	if (extra) { memcpy(p, "LIST", 4); DWORD n = 5; memcpy(p + 4, &n, 4); memcpy(p + 8, "INFOx\0", 6); p += 8 + 6; }  // odd size, padded
	memcpy(p, "fmt ", 4); DWORD n = 16; memcpy(p + 4, &n, 4);
	WORD fmt = 1, c = (WORD)ch, ba = (WORD)(ch * bits / 8), bt = (WORD)bits; DWORD bps = rate * ba;
	memcpy(p + 8, &fmt, 2); memcpy(p + 10, &c, 2); memcpy(p + 12, &rate, 4); memcpy(p + 16, &bps, 4); memcpy(p + 20, &ba, 2); memcpy(p + 22, &bt, 2);
	p += 24;
	memcpy(p, "data", 4); memcpy(p + 4, &bytes, 4); memcpy(p + 8, data, bytes); p += 8 + bytes;
	*len = (DWORD)(p - b);
	memcpy(b, "RIFF", 4); DWORD riff = *len - 8; memcpy(b + 4, &riff, 4); memcpy(b + 8, "WAVE", 4);
	return b;
}

TEST(wav_reading)
{
	short pcm[8] = { 1, -1, 2, -2, 3, -3, 4, -4 };
	DWORD len; BYTE *w = t_wav(2, 16, 44100, pcm, sizeof pcm, TRUE, &len);
	t_write("a.wav", w, len);
	char p[MAX_PATH]; t_path(p, sizeof p, "a.wav");
	BYTE *data; DWORD bytes = 0, rate = 0; WORD ch = 0, bits = 0;
	CHECK(wav_data(p, &data, &bytes, &ch, &bits, &rate));
	CHECK_EQ(bytes, sizeof pcm); CHECK_EQ(ch, 2); CHECK_EQ(bits, 16); CHECK_EQ(rate, 44100);
	CHECK(data && !memcmp(data, pcm, sizeof pcm));
	free(data);
	WAVEFORMATEX wf;
	CHECK(wav_format(p, &wf));
	CHECK_EQ(wf.nChannels, 2); CHECK_EQ(wf.nSamplesPerSec, 44100); CHECK_EQ(wf.wBitsPerSample, 16);
	t_write("cut.wav", w, len - 6);             // data cut short
	t_path(p, sizeof p, "cut.wav");
	CHECK(!wav_data(p, &data, &bytes, &ch, &bits, &rate));
	CHECK(data == NULL);
	t_write("riff.wav", "RIFF\0\0\0\0WAVX", 12);
	t_path(p, sizeof p, "riff.wav");
	CHECK(!wav_data(p, &data, &bytes, &ch, &bits, &rate));
	CHECK(!wav_format(p, &wf));
	t_path(p, sizeof p, "missing.wav");
	CHECK(!wav_data(p, &data, &bytes, &ch, &bits, &rate));
	CHECK(!wav_format(p, &wf));
	free(w);
}

TEST(wav_without_format_chunk)
{
	BYTE b[12 + 8 + 4];
	memcpy(b, "RIFF", 4); DWORD n = sizeof b - 8; memcpy(b + 4, &n, 4); memcpy(b + 8, "WAVE", 4);
	memcpy(b + 12, "data", 4); n = 4; memcpy(b + 16, &n, 4); memset(b + 20, 0, 4);
	t_write("nofmt.wav", b, sizeof b);
	char p[MAX_PATH]; t_path(p, sizeof p, "nofmt.wav");
	BYTE *data; DWORD bytes, rate; WORD ch, bits;
	CHECK(!wav_data(p, &data, &bytes, &ch, &bits, &rate));
}

TEST(music_best_offset_finds_the_match)
{
	enum { HAY = 20000, N = 2000, AT = 12345 };
	float *hay = malloc(HAY * sizeof *hay);
	for (int i = 0; i < HAY; i++) hay[i] = (float)((int)(t_rand() % 2001) - 1000);
	float score = 0;
	CHECK_EQ(best_offset(hay + AT, N, hay, 0, HAY - N, &score), AT);
	CHECK_NEAR(score, 1.0, 0.001);
	float *quiet = malloc(N * sizeof *quiet);   // the same, softer and shifted: correlation ignores both
	for (int i = 0; i < N; i++) quiet[i] = hay[AT + i] * 0.25f + 40;
	CHECK_EQ(best_offset(quiet, N, hay, 0, HAY - N, &score), AT);
	free(hay); free(quiet);
}

TEST(music_cut_markers)
{
	short pcm[4] = { 0 };
	DWORD len; BYTE *w48 = t_wav(2, 16, 48000, pcm, sizeof pcm, FALSE, &len);
	t_write("r48.wav", w48, len);
	char p[MAX_PATH]; t_path(p, sizeof p, "r48.wav");
	CHECK(music_is_cut(p, "setup.wav"));        // not made by the patch (Lords2Resounded's 48 kHz): left alone
	BYTE *w44 = t_wav(2, 16, 44100, pcm, sizeof pcm, FALSE, &len);
	t_write("r44.wav", w44, len);
	t_path(p, sizeof p, "r44.wav");
	CHECK(!music_is_cut(p, "setup.wav"));       // ours, not cut yet
	Sleep(20);
	t_write("r44.wav.cut", "x", 1);
	CHECK(music_is_cut(p, "setup.wav"));        // the marker is newer
	free(w48); free(w44);
}

TEST(music_trim_cuts_at_the_game_track)
{
	// The game's setup.wav (8-bit 11025 Hz) and a remaster that starts 1 s later: the cut must follow
	// Lords2Resounded's edit for setup.wav (0.0033 s .. 39.2855 s of the game's track), placed by the match.
	enum { GRATE = 11025, RRATE = 44100, GSEC = 40, LEAD = RRATE, TAIL = RRATE };
	int gn = GSEC * GRATE;
	BYTE *g = malloc(gn);
	for (int i = 0; i < gn; i++) g[i] = (BYTE)(128 + (int)(t_rand() % 161) - 80);
	DWORD glen; BYTE *gw = t_wav(1, 8, GRATE, g, gn, FALSE, &glen);
	t_write("setup.wav", gw, glen);
	int rn = LEAD + gn * 4 + TAIL;
	short *r = calloc((size_t)rn * 2, sizeof *r);
	for (int i = 0; i < gn * 4; i++) { short v = (short)((g[i / 4] - 128) * 200); r[(LEAD + i) * 2] = v; r[(LEAD + i) * 2 + 1] = v; }
	DWORD rlen; BYTE *rw = t_wav(2, 16, RRATE, r, (DWORD)rn * 4, FALSE, &rlen);
	t_write("lords2hd\\music\\setup.wav", rw, rlen);
	char p[MAX_PATH]; t_path(p, sizeof p, "lords2hd\\music\\setup.wav");
	music_trim(p, "setup.wav");
	BYTE *data = NULL; DWORD bytes = 0, rate = 0; WORD ch = 0, bits = 0;
	REQUIRE(wav_data(p, &data, &bytes, &ch, &bits, &rate));
	CHECK_EQ(rate, RRATE); CHECK_EQ(ch, 2); CHECK_EQ(bits, 16);
	double want = (39.2855 - 0.0033) * RRATE;  // frames
	CHECK_NEAR(bytes / 4.0, want, 2);
	// the cut starts 0.0033 s into the game's track (after the 1 s lead-in), faded in over 3 ms
	const short *o = (const short *)data;
	int start = LEAD + (int)(0.0033 * RRATE + 0.5), k = 500;   // a frame after the fade
	CHECK_EQ(o[k * 2], r[(start + k) * 2]);
	CHECK_EQ(o[0], 0);
	char mark[MAX_PATH]; snprintf(mark, sizeof mark, "%s.cut", p);
	CHECK(GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES);
	free(data); free(g); free(gw); free(r); free(rw);
}

/* ================================ checksums (multiplayer logs compare them between players) */

TEST(crc32_known_values)
{
	CHECK_EQ(~crc_block(~0u, (const BYTE *)"123456789", 9), 0xCBF43926u);  // the standard CRC-32 check value
	CHECK_EQ(~crc_block(~0u, (const BYTE *)"", 0), 0u);
	DWORD h = crc_block(~0u, (const BYTE *)"1234", 4);                      // in pieces = at once
	CHECK_EQ(~crc_block(h, (const BYTE *)"56789", 5), 0xCBF43926u);
	t_write("c.bin", "123456789", 9);
	char p[MAX_PATH]; t_path(p, sizeof p, "c.bin");
	CHECK_EQ(crc_file(p), 0xCBF43926u);
	t_path(p, sizeof p, "missing.bin");
	CHECK_EQ(crc_file(p), 0u);
}

/* ================================ small helpers */

TEST(chebyshev_distance)
{
	CHECK_EQ(cheb(0, 0, 0, 0), 0);
	CHECK_EQ(cheb(0, 0, 3, -2), 3);
	CHECK_EQ(cheb(5, 5, 1, 9), 4);
	CHECK_EQ(cheb(79, 0, 0, 79), 79);
}

/* The Town Center's ground layer takes the picture and the overlays peasants stand on, by the set's name: the file the
 * game loaded or its season file. */
TEST(town_center_ground_pieces)
{
	CHECK(tc_ground("vill.pl8", 0));
	CHECK(tc_ground("VILL.PL8", 0));
	CHECK(tc_ground("vill_winter", 0));
	CHECK(tc_ground("villani2_autumn", 40));
	CHECK(tc_ground("villani2.pl8", 41));
	CHECK(tc_ground("villani2_spring", 43));
	CHECK(!tc_ground("villani2_spring", 42));
	CHECK(!tc_ground("villani2_summer", 1));   // a quarry worker
	CHECK(!tc_ground("villani1_winter", 0));   // the iron mine's animation
	CHECK(!tc_ground("villtops_winter", 0));
	CHECK(!tc_ground("iconvill_winter", 0));
	CHECK(!tc_ground("misc_cty.pl8", 1));      // the idle serf
	CHECK(!tc_ground("", 0));
}
