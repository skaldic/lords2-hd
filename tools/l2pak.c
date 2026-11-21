// Packs the HD art (<game>\lords2hd\screens, wide, tiles, sprites; the restored voices in sound; the
// disc-quality videos in videos; the videos' 2x frames in videohd)
// into lords2hd.pak, the one file the DLL reads it from (see "The art pack" in hd/lords2hd.c for the
// format: a header, the files one after another, and an index; plain, not scrambled).
// Built and run by tools\make_pak.ps1:
//   l2pak <lords2hd folder> <out.pak>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define PAK_MAGIC 0x50483243
#define PAK_VERSION 2
typedef struct { char name[48]; DWORD off, size; } PAK_ENTRY;
static PAK_ENTRY entries[4096];
static int count;

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: l2pak <lords2hd folder> <out.pak>\n"); return 2; }
	FILE *out = fopen(argv[2], "wb");
	if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
	DWORD head[4] = { PAK_MAGIC, 0, 0, PAK_VERSION }; // magic, count, index offset, version
	fwrite(head, sizeof head, 1, out);
	static const char *dirs[] = { "screens", "wide", "tiles", "sprites", "sound", "videos", "videohd" }; // sound, videos, videohd: optional
	static const char *exts[] = { NULL, NULL, NULL, NULL, ".wav", ".smk", ".bin" }; // the first four: .bin and .pal
	static BYTE buf[1 << 20];
	unsigned long long total = 0;
	for (int d = 0; d < 7; d++) {
		char pattern[MAX_PATH];
		snprintf(pattern, sizeof pattern, "%s\\%s\\*", argv[1], dirs[d]);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(pattern, &fd);
		if (h == INVALID_HANDLE_VALUE) { if (d >= 4) continue; fprintf(stderr, "no folder %s\\%s\n", argv[1], dirs[d]); return 1; }
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			const char *ext = strrchr(fd.cFileName, '.');
			if (!ext || (d < 4 ? _stricmp(ext, ".bin") && _stricmp(ext, ".pal") : _stricmp(ext, exts[d]))) continue;
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
	if (idx + (long long)count * (long long)sizeof(PAK_ENTRY) > 0xffffffffLL) { fprintf(stderr, "pack over 4 GB\n"); return 1; } // (offsets are 32-bit)
	fwrite(entries, sizeof(PAK_ENTRY), count, out);
	head[1] = (DWORD)count; head[2] = (DWORD)idx;
	fseek(out, 0, SEEK_SET);
	fwrite(head, sizeof head, 1, out);
	fclose(out);
	printf("%d files, %llu MB packed into %s\n", count, total >> 20, argv[2]);
	return 0;
}
