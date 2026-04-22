// tree.c - Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Mode constants
#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int path_has_prefix(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0;
}

static int tree_has_entry_name(const Tree *tree, const char *name) {
    for (int i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int write_tree_level(const Index *index, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const char *full_path = index->entries[i].path;
        if (!path_has_prefix(full_path, prefix)) continue;

        const char *rest = full_path + prefix_len;
        if (*rest == '\0') continue;

        const char *slash = strchr(rest, '/');
        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = index->entries[i].mode;
            entry->hash = index->entries[i].hash;
            snprintf(entry->name, sizeof(entry->name), "%s", rest);
            continue;
        }

        size_t dir_len = (size_t)(slash - rest);
        if (dir_len == 0 || dir_len >= sizeof(tree.entries[0].name)) return -1;

        char dirname[256];
        memcpy(dirname, rest, dir_len);
        dirname[dir_len] = '\0';

        if (tree_has_entry_name(&tree, dirname)) continue;
        if (tree.count >= MAX_TREE_ENTRIES) return -1;

        char child_prefix[512];
        if (snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, dirname) >= (int)sizeof(child_prefix)) {
            return -1;
        }

        ObjectID child_id;
        if (write_tree_level(index, child_prefix, &child_id) != 0) return -1;

        TreeEntry *entry = &tree.entries[tree.count++];
        entry->mode = MODE_DIR;
        entry->hash = child_id;
        snprintf(entry->name, sizeof(entry->name), "%s", dirname);
    }

    void *serialized = NULL;
    size_t serialized_len = 0;
    if (tree_serialize(&tree, &serialized, &serialized_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, serialized, serialized_len, id_out);
    free(serialized);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    return write_tree_level(&index, "", id_out);
}
