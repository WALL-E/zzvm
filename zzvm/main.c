#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzvm.h"

#define ZZ_IMAGE_MAGIC   0x7a5a /* 'Zz' */
#define ZZ_IMAGE_VERSION 0x0

typedef struct __attribute__((__packed__)) {
    ZZ_ADDRESS section_addr;
    ZZ_ADDRESS section_size;
} ZZ_SECTION_HEADER;

typedef struct __attribute__((__packed__)) {
    uint16_t          magic;
    uint16_t          file_ver;
    ZZ_ADDRESS        entry;
    uint16_t          section_count;
    ZZ_SECTION_HEADER sections[0];
} ZZ_IMAGE_HEADER;

// decode a byte of Zz-encoded data (encoded) to buffer (out)
int zz_decode_byte(const char *encoded, uint8_t *out)
{
    uint8_t value = 0;

    for(int i = 0; i < 8; i++)
    {
        value <<= 1;
        if(encoded[i] == 'Z') {
            value |= 1;
        } else if(encoded[i] != 'z') {
            return 0;
        }
    }

    *out = value;
    return 1;
}

// decode Zz-encoded data from source (src) to buffer (dst)
int zz_decode_data(void *dst, const void *src, size_t unpacked_size)
{
    uint8_t *dst8 = (uint8_t *)dst;
    uint64_t i, val, packed_size;

    for(i = 0; i < unpacked_size; i++) {
        if(!zz_decode_byte(src + i * 8, &dst8[i])) {
            return 0;
        }
    }

    return 1;
}

// read and decode header data from file (fp) to buffer (header)
int zz_read_image_header(FILE *fp, ZZ_IMAGE_HEADER *header)
{
    char buffer[sizeof(*header) * 8];

    if(fread(buffer, sizeof(buffer), 1, fp) != 1) {
        fprintf(stderr, "Unable to read file\n");
        return 0;
    }

    if(!zz_decode_data(header, buffer, sizeof(*header))) {
        fprintf(stderr, "Malformed file\n");
        return 0;
    }

    return 1;
}

// verify header data (header), checking magic number and file version
int zz_verify_image_header(ZZ_IMAGE_HEADER *header)
{
    if(header->magic != ZZ_IMAGE_MAGIC) {
        fprintf(stderr, "Invalid file magic (%.4x)\n", header->magic);
        return 0;
    }

    if(header->file_ver != ZZ_IMAGE_VERSION) {
        fprintf(stderr, "Mismatch file version\n");
        return 0;
    }

    return 1;
}

// read and decode header->sections
int zz_read_image_header_section(FILE *fp, ZZ_SECTION_HEADER *section)
{
    char buffer[sizeof(*section) * 8];

    if(fread(buffer, sizeof(buffer), 1, fp) != 1) {
        fprintf(stderr, "Can not read file section\n");
        return 0;
    }

    if(!zz_decode_data(section, buffer, sizeof(*section))) {
        fprintf(stderr, "Malformed file\n");
        return 0;
    }

    return 1;
}

// read and decode ZZ_IMAGE_HEADER
int zz_load_image_header(FILE *fp, ZZ_IMAGE_HEADER **out_header)
{
    ZZ_IMAGE_HEADER *header = (ZZ_IMAGE_HEADER *)malloc(sizeof(ZZ_IMAGE_HEADER));

    if(!zz_read_image_header(fp, header)) {
        return 0;
    }
    if(!zz_verify_image_header(header)) {
        return 0;
    }

    header = (ZZ_IMAGE_HEADER *)realloc(header, sizeof(ZZ_IMAGE_HEADER) +
            sizeof(ZZ_SECTION_HEADER) * header->section_count);

    for(int i = 0; i < header->section_count; i++) {
        if(!zz_read_image_header_section(fp, &header->sections[i])) {
            return 0;
        }
    }

    *out_header = header;
    return 1;
}

// read, decode image and put things into an existed vm
int zz_load_image_to_vm(const char *filename, ZZVM *vm, ZZ_IMAGE_HEADER **out_header)
{
    FILE *fp;
    ZZ_IMAGE_HEADER *header = NULL;

    if(strcmp(filename, "-") == 0 || filename == NULL) {
        fp = stdin;
    } else {
        fp = fopen(filename, "rb");
    }

    if(!fp) {
        fprintf(stderr, "Unable to open file\n");
        return 0;
    }

    if(!zz_load_image_header(fp, &header)) {
        return 0;
    }

    vm->ctx.regs.IP = header->entry;

    size_t buffer_size = 8192;
    char *buffer = malloc(buffer_size);

    for(int i = 0; i < header->section_count; i++) {
        ZZ_SECTION_HEADER *section_header = &header->sections[i];

        size_t size_bound = (size_t)section_header->section_addr +
                            (size_t)section_header->section_size;
        size_t encoded_size = section_header->section_size * 8;

        if(size_bound >= sizeof(vm->ctx.memory)) {
            fprintf(stderr, "Section#%d out of scope\n", i);
            goto fail;
        }

        if(buffer_size < encoded_size) {
            buffer_size = encoded_size;
            buffer = realloc(buffer, buffer_size);
        }

        if(fread(buffer, encoded_size, 1, fp) != 1) {
            fprintf(stderr, "Can not read section #%d\n", i);
            goto fail;
        }

        if(!zz_decode_data(&vm->ctx.memory[section_header->section_addr], buffer, section_header->section_size)) {
            fprintf(stderr, "Malformed file\n");
            goto fail;
        }
    }

    if(fp != stdin) fclose(fp);
    if(out_header) {
        *out_header = header;
    } else {
        free(header);
    }
    free(buffer);

    return 1;

fail:
    if(fp && fp != stdin) fclose(fp);
    if(header) free(header);
    if(buffer) free(buffer);
    return 0;
}

// dump vm context and print
void dump_vm_context(ZZVM *vm)
{
    char buffer[1024];
    zz_dump_context(&vm->ctx, buffer, sizeof(buffer));
    puts(buffer);
}

// load zz-image into vm and run
int run_file(const char *filename, int trace)
{
    ZZVM *vm;
    if(zz_create(&vm) != ZZ_SUCCESS) {
        fprintf(stderr, "Can not create vm\n");
        return 0;
    }

    if(!zz_load_image_to_vm(filename, vm, NULL)) {
        return 0;
    }

    zz_msg_pipe = stderr;

    int stop_reason = ZZ_SUCCESS;
    while(stop_reason != ZZ_HALT) {
        int ret_val = zz_execute(vm, 1, &stop_reason);

        if(trace) {
            char buffer[64];
            ZZ_INSTRUCTION *ins = zz_fetch(&vm->ctx);
            zz_disasm(vm->ctx.regs.IP, ins, buffer, sizeof(buffer) - 1);
            fprintf(zz_msg_pipe, "[TRACE] %.4x: %s\n", vm->ctx.regs.IP, buffer);
            dump_vm_context(vm);
        }

        if(ret_val != ZZ_SUCCESS) {
            fprintf(zz_msg_pipe, "Failed to execute, stop_reason = %d\n", stop_reason);
            break;
        }
    }

    zz_destroy(vm);
    return 1;
}

// disassemble zz-image file
int disassemble_file(const char *filename)
{
    ZZVM *vm;
    if(zz_create(&vm) != ZZ_SUCCESS) {
        fprintf(stderr, "Can not create vm\n");
        return 0;
    }

    ZZ_IMAGE_HEADER *header;

    if(!zz_load_image_to_vm(filename, vm, &header)) {
        return 0;
    }

    for(int i = 0; i < header->section_count; i++) {
        ZZ_SECTION_HEADER *section = &header->sections[i];
        ZZ_ADDRESS addr = section->section_addr;
        ZZ_ADDRESS addr_end = addr + section->section_size;

        printf("disassembly for section #%d from 0x%.4x to 0x%.4x\n", i, addr, addr_end);
        puts("");

        if(addr_end > ZZ_MEM_LIMIT - sizeof(ZZ_INSTRUCTION) || addr_end < addr) {
            addr_end = ZZ_MEM_LIMIT - sizeof(ZZ_INSTRUCTION);
        }

        char buffer[128];

        while(addr < addr_end) {
            ZZ_INSTRUCTION* ins = (ZZ_INSTRUCTION *)&vm->ctx.memory[addr];
            if(zz_disasm(addr, ins, buffer, sizeof(buffer)) != ZZ_SUCCESS) {
                fprintf(stderr, "Can not disassemble at address %.4x\n", addr);
                break;
            }
            printf("%.4x %s\n", addr, buffer);
            addr += sizeof(ZZ_INSTRUCTION);
        }

        puts("");
    }

    free(header);
    zz_destroy(vm);
    return 1;
}

void usage(const char *prog)
{
    printf("zzvm\n\n"
           "  zz virtual machine by Inndy\n\n"
           "Feature:\n\n"
#ifdef ZZ_UNIX_ENV
           "  UNIX Env: Yes\n"
#else
           "  UNIX Env: No\n"
#endif
           "\n"
           "Usage: %s <command> zz-image\n\n"
           "  available command:\n"
           "    run\n"
           "      run until HLT instruction\n"
           "    trace\n"
           "      run one step and dump context until HLT instruction\n"
           "    disasm\n"
           "      disassemble a zz file\n"
           , prog);
}

int main(int argc, const char * const argv[])
{
    if(argc < 3) {
        usage(argv[0]);
    } else if(argc >= 3) {
        if(strcmp(argv[1], "trace") == 0) {
            run_file(argv[2], 1);
        } else if(strcmp(argv[1], "run") == 0) {
            run_file(argv[2], 0);
        } else if(strcmp(argv[1], "disasm") == 0) {
            disassemble_file(argv[2]);
        } else {
            printf("Unknow command %s\n", argv[1]);
        }
    }
}
