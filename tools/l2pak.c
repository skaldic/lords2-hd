// Packs the HD art (<game>\lords2hd\screens, wide, tiles, sprites, and the restored voices in sound)
// into lords2hd.pak, the one file the
// DLL reads it from (see "The art pack" in hd/lords2hd.c for the format and what the scrambling is
// and is not). Built and run by tools\make_pak.ps1:
//   l2pak <lords2hd folder> <out.pak>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define PAK_MAGIC 0x50483243
typedef struct { char name[48]; DWORD off, size; } PAK_ENTRY;
static PAK_ENTRY entries[4096];
static int count;

static DWORD key(DWORD *s) { DWORD x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: l2pak <lords2hd folder> <out.pak>\n"); return 2; }
	BYTE fw[256];
	DWORD s = 0x2545f491u;
	for (int i = 0; i < 256; i++) fw[i] = (BYTE)i;
	for (int i = 255; i > 0; i--) { int j = key(&s) % (i + 1); BYTE t = fw[i]; fw[i] = fw[j]; fw[j] = t; }
	FILE *out = fopen(argv[2], "wb");
	if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
	DWORD head[4] = { PAK_MAGIC, 0, 0, 1 };
	fwrite(head, sizeof head, 1, out);
	static const char *dirs[] = { "screens", "wide", "tiles", "sprites", "sound" }; // sound: optional (restored voices)
	static BYTE buf[1 << 20];
	unsigned long long total = 0;
	for (int d = 0; d < 5; d++) {
		char pattern[MAX_PATH];
		snprintf(pattern, sizeof pattern, "%s\\%s\\*", argv[1], dirs[d]);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(pattern, &fd);
		if (h == INVALID_HANDLE_VALUE) { if (d == 4) continue; fprintf(stderr, "no folder %s\\%s\n", argv[1], dirs[d]); return 1; }
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			const char *ext = strrchr(fd.cFileName, '.');
			if (!ext || (d < 4 ? _stricmp(ext, ".bin") && _stricmp(ext, ".pal") : _stricmp(ext, ".wav"))) continue;
			if (count >= 4096) { fprintf(stderr, "too many files\n"); return 1; }
			PAK_ENTRY *e = &entries[count];
			memset(e, 0, sizeof *e);
			if (snprintf(e->name, sizeof e->name, "%s/%s", dirs[d], fd.cFileName) >= (int)sizeof e->name) { fprintf(stderr, "name too long: %s\n", fd.cFileName); return 1; }
			for (char *c = e->name; *c; c++) *c = (char)tolower((BYTE)*c);
			char path[MAX_PATH];
			snprintf(path, sizeof path, "%s\\%s\\%s", argv[1], dirs[d], fd.cFileName);
			FILE *in = fopen(path, "rb");
			if (!in) { fprintf(stderr, "cannot read %s\n", path); return 1; }
			long long pos = _ftelli64(out);
			if (pos > 0xffffffffLL) { fprintf(stderr, "pack over 4 GB\n"); return 1; }
			e->off = (DWORD)pos;
			size_t n;
			while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
				for (size_t i = 0; i < n; i++) buf[i] = fw[buf[i]];
				fwrite(buf, 1, n, out);
				e->size += (DWORD)n;
			}
			fclose(in);
			total += e->size;
			count++;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	long long idx = _ftelli64(out);
	s = 0x9e3779b9u ^ (DWORD)count;
	for (int i = 0; i < count * (int)sizeof(PAK_ENTRY) / 4; i++) ((DWORD *)entries)[i] ^= key(&s);
	fwrite(entries, sizeof(PAK_ENTRY), count, out);
	head[1] = (DWORD)count; head[2] = (DWORD)idx;
	fseek(out, 0, SEEK_SET);
	fwrite(head, sizeof head, 1, out);
	fclose(out);
	printf("%d files, %llu MB packed into %s\n", count, total >> 20, argv[2]);
	return 0;
}
