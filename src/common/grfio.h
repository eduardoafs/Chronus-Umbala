// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef	_GRFIO_H_
#define	_GRFIO_H_

void grfio_init(char*);			// GRFIO Initialize
void grfio_final(void);			// GRFIO Finalize
int grfio_add(char*);			// GRFIO Resource file add
void* grfio_read(char*);		// GRFIO data file read
void* grfio_reads(char*,int*);	// GRFIO data file read & size get
int grfio_size(char*);			// GRFIO data file size get
unsigned long grfio_crc32(const char *buf, unsigned int len);

int decode_zip(unsigned char *dest, unsigned long* destLen, const unsigned char* source, unsigned long sourceLen);
int encode_zip(unsigned char *dest, unsigned long* destLen, const unsigned char* source, unsigned long sourceLen);
int decode_file (FILE *source, FILE *dest);

int deflate_file (const char *source, const char *filename);

#endif	// _GRFIO_H_
