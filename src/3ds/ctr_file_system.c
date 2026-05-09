#include "ctr_file_system.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef LOG_ALL
#define CTR_FS_LOG(...) do { \
    fprintf(stderr, "[CTR_FS] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    fflush(stderr); \
} while (0)
#else
#define CTR_FS_LOG(...) ((void)0)
#endif

static char *buildFullPath(N3dsFileSystem *fs, const char *relativePath) {
    if (strncmp(relativePath, fs->basePath, strlen(fs->basePath)) == 0) {
        return safeStrdup(relativePath);
    }

    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char *fullPath = safeMalloc(baseLen + relLen + 1);

    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';

    return fullPath;
}

static char *n3dsResolvePath(FileSystem *fs, const char *relativePath) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    CTR_FS_LOG("resolve '%s' -> '%s'", relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)");
    return fullPath;
}

static bool n3dsFileExists(FileSystem *fs, const char *relativePath) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    CTR_FS_LOG("exists %s '%s' -> '%s'%s%s", exists ? "yes" : "no",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               exists ? "" : " errno=", exists ? "" : strerror(errno));
    free(fullPath);
    return exists;
}

static char *n3dsReadFileText(FileSystem *fs, const char *relativePath) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    FILE *f = fopen(fullPath, "rb");

    if (f == NULL) {
        CTR_FS_LOG("read_text fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    CTR_FS_LOG("read_text ok '%s' -> '%s' size=%ld read=%lu",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               size, (unsigned long) bytesRead);
    free(fullPath);

    return content;
}

static bool n3dsWriteFileText(FileSystem *fs, const char *relativePath, const char *contents) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    FILE *f = fopen(fullPath, "wb");

    if (f == NULL) {
        CTR_FS_LOG("write_text open fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return false;
    }

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    CTR_FS_LOG("write_text %s '%s' -> '%s' size=%lu written=%lu",
               written == len ? "ok" : "short", relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               (unsigned long) len, (unsigned long) written);
    free(fullPath);

    return written == len;
}

static bool n3dsDeleteFile(FileSystem *fs, const char *relativePath) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    int result = remove(fullPath);
    CTR_FS_LOG("delete %s '%s' -> '%s'%s%s", result == 0 ? "ok" : "fail",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               result == 0 ? "" : " errno=", result == 0 ? "" : strerror(errno));
    free(fullPath);
    return result == 0;
}

static bool n3dsReadFileBinary(FileSystem *fs, const char *relativePath, uint8_t **outData, int32_t *outSize) {
    *outData = NULL;
    *outSize = 0;

    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    FILE *f = fopen(fullPath, "rb");
    if (f == NULL) {
        CTR_FS_LOG("read_bin fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        CTR_FS_LOG("read_bin size fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        fclose(f);
        free(fullPath);
        return false;
    }

    uint8_t *data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);
    if (bytesRead != (size_t) size) {
        CTR_FS_LOG("read_bin short '%s' -> '%s' size=%ld read=%lu",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
                   size, (unsigned long) bytesRead);
        free(data);
        free(fullPath);
        return false;
    }

    *outData = data;
    *outSize = (int32_t) size;
    CTR_FS_LOG("read_bin ok '%s' -> '%s' size=%ld",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", size);
    free(fullPath);
    return true;
}

static bool n3dsWriteFileBinary(FileSystem *fs, const char *relativePath, const uint8_t *data, int32_t size) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    FILE *f = fopen(fullPath, "wb");
    if (f == NULL) {
        CTR_FS_LOG("write_bin open fail '%s' -> '%s' size=%d errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", size, strerror(errno));
        free(fullPath);
        return false;
    }

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    CTR_FS_LOG("write_bin %s '%s' -> '%s' size=%d written=%lu",
               written == (size_t) size ? "ok" : "short",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               size, (unsigned long) written);
    free(fullPath);
    return written == (size_t) size;
}

static FileSystemVtable n3dsFileSystemVtable = {
    .resolvePath = n3dsResolvePath,
    .fileExists = n3dsFileExists,
    .readFileText = n3dsReadFileText,
    .writeFileText = n3dsWriteFileText,
    .deleteFile = n3dsDeleteFile,
    .readFileBinary = n3dsReadFileBinary,
    .writeFileBinary = n3dsWriteFileBinary,
};

N3dsFileSystem *N3dsFileSystem_create(const char *dataWinPath) {
    N3dsFileSystem *fs = safeCalloc(1, sizeof(N3dsFileSystem));
    fs->base.vtable = &n3dsFileSystemVtable;

    const char *lastSlash = strrchr(dataWinPath, '/');

    if (lastSlash != NULL) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("sdmc:/");
    }

    CTR_FS_LOG("create dataWin='%s' basePath='%s'", dataWinPath ? dataWinPath : "(null)", fs->basePath ? fs->basePath : "(null)");
    return fs;
}

void N3dsFileSystem_destroy(N3dsFileSystem *fs) {
    if (fs == NULL) return;
    CTR_FS_LOG("destroy basePath='%s'", fs->basePath ? fs->basePath : "(null)");
    free(fs->basePath);
    free(fs);
}
