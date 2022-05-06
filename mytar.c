#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <err.h>


#define BLOCK_SIZE  512
#define MAGIC       "ustar  "
#define REG_FILE    "0"


// POSIX tar header
struct Header
{
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
};


// only listing and extracting files are now supported
enum mode
{
    LIST,
    EXTRACT
};


// computes decimal number from string representation of octal number
long oct2dec(char *octal)
{
    long decimal = 0;
    long radix = 1;
    int length = strlen(octal);

    for (int i = length - 1; i >= 0; i--)
    {
        decimal += (octal[i] - '0') * radix;
        radix *= 8;
    }

    return decimal;
}


long get_archive_size(FILE *fin)
{
    fseek(fin, 0, SEEK_END);
    long size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    return size;
}


// returns true if block contains only zero bytes
bool is_empty_block(char *buffer)
{
    for (int i = 0; i < BLOCK_SIZE; i++)
    {
        if (buffer[i] != '\0')
        {
            return false;
        }
    }

    return true;
}


// check "magic" field in header
void is_tar_archive(struct Header *header)
{
    if (strcmp(header->magic, MAGIC) != 0)
    {
        warnx("This does not look like a tar archive");
        errx(2, "Exiting with failure status due to previous errors");
    }
}


// only regular files are supported
void is_regular_file(struct Header *header)
{
    if (strcmp(header->typeflag, REG_FILE) != 0)
    {
        errx(2, "Unsupported header type: %d", header->typeflag[0]);
    }
}


// if filename appears among arguments to be listed, mark the file as found (1)
// and return true (so that filename will be printed)
bool mark_file(char *filename, char **files_args, int files_count, bool *files_found)
{
    for (int i = 0; i < files_count; i++)
    {
        if (strcmp(filename, files_args[i]) == 0 && !files_found[i])
        {
            files_found[i] = true;
            return true;
        }
    }

    return false;
}


// reports files that were not found in the archive
// returns true if any of specified files was NOT present in archive
bool report_missing_files(char **files_args, int files_count, bool *files_found)
{
    bool was_found = false;

    for (int i = 0; i < files_count; i++)
    {
        if (!files_found[i])
        {
            warnx("%s: Not found in archive", files_args[i]);
            was_found = true;
        }
    }

    return was_found;
}


void extract_file(FILE *fin, char *buffer)
{
    struct Header *header = (struct Header*)buffer;
    FILE *fout = fopen(header->name, "w");

    if (fout == NULL)
    {
        errx(2, "Error creating file");
    }

    long file_size = oct2dec(header->size);                             // size of file in the current entry
    long blocks_count = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;      // number of blocks containing file data
    long bytes_left = file_size - (blocks_count - 1) * BLOCK_SIZE;      // data bytes remaning in the last block

    // read and write each data block except the last one
    // last block may be padded with zeros
    // we need to write only remaining data bytes from the last block
    for (int i = 0; i < blocks_count; i++)
    {
        // whole block was read successfully
        if (fread(buffer, BLOCK_SIZE, 1, fin) == 1)
        {
            // last block
            if (i + 1 == blocks_count)
            {
                fwrite(buffer, bytes_left, 1, fout);
            }
            else
            {
                fwrite(buffer, BLOCK_SIZE, 1, fout);
            }
        }
        else
        {
            warnx("Unexpected EOF in archive");
            errx(2, "Error is not recoverable: exiting now");
        }
    }

    fclose(fout);
}


// reads whole archive and compare every filename with arguments
// prints filenames and extracts files if needed
void read_archive(FILE *fin, long archive_size, char **files_args, int files_count, enum mode action, bool verbose)
{
    struct Header *header;
    char buffer[BLOCK_SIZE];
    long file_size;                             // size of archived file according to current header
    long blocks_count;                          // number of blocks with contents of file
    bool first_empty = false;                   // first zero block encountered
    bool second_empty = false;                  // second zero block encountered
    int blocks_read = 0;                        // number of blocks read


    // used for evidence which files were found in the archive
    bool *files_found = calloc(files_count, sizeof(bool));

    if (files_found == NULL)
    {
        errx(2, "calloc");
    }

    while (fread(&buffer, BLOCK_SIZE, 1,  fin) == 1)
    {
        blocks_read++;

        if (is_empty_block(buffer))
        {
            if (!first_empty)
            {
                first_empty = true;
                continue;
            }
            else
            {
                second_empty = true;            // two consecutive empty blocks signalize the end of archive
                break;
            }
        }

        header = (struct Header*)&buffer;
        file_size = oct2dec(header->size);                          // size of file in the current entry
        blocks_count = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;   // number of blocks containing file data, rounded up

        is_tar_archive(header);                 // check magic field in header
        is_regular_file(header);                // check typeflag field in header

        // filename was found among arguments
        bool filename_found = mark_file(header->name, files_args, files_count, files_found);
        
        // if there are no arguments or filename was among them, it may be printed
        bool should_print = (files_count == 0 || filename_found) ? true : false;

        // when in listing mode, print filename
        // when in extraction mode, print filename if verbose flag is also set 
        if (should_print && (action == LIST || (action == EXTRACT && verbose)))
        {
            printf("%s\n", header->name);
        }

        fflush(stdout);

        // when in extraction mode, extract file from current entry
        // else advance to the next file header
        if (action == EXTRACT && should_print)
        {
            extract_file(fin, buffer);
        }
        else
        {
            fseek(fin, BLOCK_SIZE * blocks_count, SEEK_CUR);
        }

        // if fseek got beyond the end of file, report an error
        if (ftell(fin) > archive_size)
        {
            warnx("Unexpected EOF in archive");
            errx(2, "Error is not recoverable: exiting now");
        }

        blocks_read += blocks_count;
    }

    // one empty block triggers a warning
    if (first_empty && !second_empty)
    {
        warnx("A lone zero block at %d", blocks_read);
    }

    // print files not found in archive
    if (report_missing_files(files_args, files_count, files_found))
    {
        errx(2, "Exiting with failure status due to previous errors");
    }

    free(files_args);
    free(files_found);
}


int main(int argc, char **argv)
{
    if (argc < 2)
    {
        errx(2, "Insufficient number of arguments");
    }
    
    FILE *fin;
    long archive_size;

    bool fflag = false;
    bool tflag = false;
    bool xflag = false;
    bool vflag = false;
    
    int files_count = 0;

    char *filename;                                         // archive name
    char **files_args = malloc(sizeof(char*) * argc);       // files to be listed/extracted, supplied as arguments


    if (files_args == NULL)
    {
        errx(2, "malloc");
    }


    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-')
        {
            switch (argv[i][1])
            {
                case 'f':
                    fflag = true;

                    if (i + 1 == argc)
                    {
                        errx(2, "Option requires an argument -- 'f'");
                    }

                    filename = argv[++i];
                    break;
                case 't':
                    tflag = true;
                    break;
                case 'x':
                    xflag = true;
                    break;
                case 'v':
                    vflag = true;
                    break;
                default:
                    errx(2, "Unknown option");
            }
        }
        else if (tflag || xflag)
        {
            files_args[files_count++] = argv[i];
        }
    }

    if (!fflag)
    {
        warnx("Refusing to read archive contents from terminal");
        errx(2, "Error is not recoverable: exiting now");
    }

    if (tflag + xflag != 1)
    {
        errx(2, "You must specify either -t  or -x option");    
    }

    if ((fin = fopen(filename, "r")) == NULL)
    {
        errx(2, "Error opening file");
    }

    enum mode action = tflag ? LIST : EXTRACT;

    archive_size = get_archive_size(fin);
    read_archive(fin, archive_size, files_args, files_count, action, vflag);

    fclose(fin);
}
