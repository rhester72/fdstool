#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// fdstool - FDS (Famicom Disk System) and QD (QuickDisk) image converter and analyzer
// Heavily inspired by and sourced from https://wiki.nesdev.com/w/index.php/Family_Computer_Disk_System

// TODO
// Determine why one CRC mismatches in Nintendo dump
// Find more pricing data (see pricing section)
// Am I handling "Boot read file code" correctly?
// Are there other game types? (00, FF, 46, 31, 44, 4B, 49)
// Are there other country codes? (4F, FF) - 00 is probably just 'unspecified' (as are several other unknowns?)
// What is a file kind of 0x10?

#define VERSION "0.1 beta"

#define QD 1
#define FDS 2

FILE *fin, *fout;

void bailout() {
	if (fin)
		fclose(fin);
	if (fout)
		fclose(fout);
	exit(2);
}

void usage(char *progname) {
	printf("fdstool %s\n\n", VERSION);
	printf("Usage: %s [opts] infile <outfile>\n\n", progname);
	printf("       -a: Add FDS header (converts from FDS to FDS)\n");
	printf("       -h: Help\n");
	printf("       -o: Overwrite outfile if it exists\n");
	printf("       -r: Remove FDS header (from FDS outfile)\n");
	printf("       -z: Zero out disk info block CRC when converting to QD (sometime used by Nintendo, e.g. Famicom Mini)\n");
	printf("\n");
	printf("If outfile is not specified, detailed information will be displayed about the contents of infile\n");
	bailout();
}

// Taken from https://forums.nesdev.com/viewtopic.php?f=2&t=15895&start=0#p194867
uint16_t gen_qd_crc(uint8_t* data, unsigned size) {
	uint16_t sum = 0x8000;

	for (unsigned byte_index = 0; byte_index < size + 2; byte_index++) {
		uint8_t byte = byte_index < size ? data[byte_index] : 0x00;
		for (unsigned bit_index = 0; bit_index < 8; bit_index++) {
			bool bit = (byte >> bit_index) & 1;
			bool carry = sum & 1;
			sum = (sum >> 1) | (bit << 15);
			if (carry) sum ^= 0x8408;
		}
	}
	return sum;
}

unsigned int hextoint(uint8_t hex) {
	char local_int[3];
	sprintf(local_int, "%x", hex);
	return atoi(local_int);
}

void print_date(uint8_t *date) {
	int year;

	if (date[0] == 0x00 || date[0] == 0xFF)
		printf("<unknown>\n");
	else {
		switch (date[1]) {
		case 0x01:
			printf("January ");
			break;
		case 0x02:
			printf("February ");
			break;
		case 0x03:
			printf("March ");
			break;
		case 0x04:
			printf("April ");
			break;
		case 0x05:
			printf("May ");
			break;
		case 0x06:
			printf("June ");
			break;
		case 0x07:
			printf("July ");
			break;
		case 0x08:
			printf("August ");
			break;
		case 0x09:
			printf("September ");
			break;
		case 0x10:
			printf("October ");
			break;
		case 0x11:
			printf("November ");
			break;
		case 0x12:
			printf("December ");
			break;
		default:
			printf("<unknown>\n");
			return;
		}
		printf("%d, ", hextoint(date[2]));
		year = hextoint(date[0]);
		if (year < 83)
			year += 1925;
		else
			year += 1900;
		printf("%d\n", year);
	}
}

int main(int argc, char **argv) {
	uint8_t buffer[65536], crc_buffer[2];
	char *infile = NULL, *outfile = NULL;
	int x, length, file_amount, file_size, total_read, total_write, source;
	int verbose = 0, dib_zero = 0, append_header = 0, overwrite = 0, remove_header = 0, header_sides = 0, total_sides = 0, rc = 0, dest = 0;
	uint16_t crc_read, crc_calc;
	const uint8_t fds_header[17] = "FDS\x1a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
	const uint8_t bios_string[16] = "\x01*NINTENDO-HVC*";

	for (x = 1; x < argc; x++) {
		if (strncmp(argv[x], "-", 1) == 0)
			for (unsigned int y = 1; y < strlen(argv[x]); y++) {
				switch (argv[x][y]) {
				case 'a':
					append_header = 1;
					break;
				case 'o':
					overwrite = 1;
					break;
				case 'r':
					remove_header = 1;
					break;
				case 'z':
					dib_zero = 1;
					break;
				default:
					usage(argv[0]);
				}
			}
		else {
			if (!infile)
				infile = argv[x];
			else
				if (!outfile)
					outfile = argv[x];
				else
					usage(argv[0]);
		}
	}

	if (append_header && remove_header) {
		printf("Can't add header if no header specified\n");
		usage(argv[0]);
	}

	if (!infile)
		usage(argv[0]);

	if (!outfile)
		verbose = 1;

	fin = fopen(infile, "rb");
	if (!fin) {
		printf("FATAL: could not open infile \"%s\"\n", infile);
		bailout();
	}
	fseek(fin, 0L, SEEK_END);
	length = ftell(fin);
	rewind(fin);
	if (length % 65536 == 0) {
		source = QD;
		if (verbose)
			printf("Source is in QD format\n");
		if (append_header) {
			printf("ERROR: Cannot append FDS header, source is QD\n");
			bailout();
		}
	}
	else if (length % 65500 == 0 || length % 65500 == 16) {
		source = FDS;
		if (verbose)
			printf("Source is in FDS format\n");
	}
	else {
		printf("FATAL: infile \"%s\" not in qd/fds format\n", infile);
		bailout();
	}

	if (outfile)
		if (source == QD || append_header || remove_header)
			dest = FDS;
		else
			dest = QD;

	if (dest == FDS && dib_zero) {
		printf("FATAL: cannot zero dib crc for fds outfile\n");
		bailout();
	}

	if (outfile) {
		if (!overwrite) {
			fout = fopen(outfile, "r");
			if (fout) {
				printf("FATAL: outfile \"%s\" exists (see -o)\n", outfile);
				bailout();
			}
		}
		fout = fopen(outfile, "wb");
		if (!fout) {
			printf("FATAL: could not open outfile \"%s\" for write\n", outfile);
			bailout();
		}
		if (dest == FDS && !append_header && !remove_header) {
			fwrite(fds_header, 16, 1, fout);
			if (verbose)
				printf("Wrote FDS header\n");
		}
	}
	else {
		if (append_header) {
			printf("FATAL: adding header requires outfile\n");
			bailout();
		}
		if (overwrite) {
			printf("FATAL: cannot overwrite if no outfile\n");
			bailout();
		}
	}

	if (source == FDS) {
		if (!fread(buffer, 16, 1, fin)) {
			printf("FATAL: fread failure for fds header\n");
			bailout();
		}
		if (memcmp(buffer, fds_header, 4) == 0 && memcmp(buffer + 5, fds_header + 5, 11) == 0) {
			// FDS header found
			header_sides = buffer[4];
			if (verbose) {
				printf("Found FDS header with %d side", header_sides);
				if (header_sides > 1)
					printf("s");
				printf("\n");
			}
			if (append_header) {
				if (verbose)
					printf("Copying header\n");
				fwrite(buffer, 16, 1, fout);
			}
		}
		else {
			// assume no FDS header
			rewind(fin);
			if (verbose)
				printf("No FDS header found\n");
			if (append_header) {
				if (verbose)
					printf("Writing new header\n");
				fwrite(fds_header, 16, 1, fout);
			}
		}
	}

	while (fread(buffer, 56, 1, fin)) {
		total_sides++;
		if (verbose) {
			printf("Disk %d Side ", (total_sides + 1) / 2);
			if (total_sides % 2 == 0)
				printf("B\n");
			else
				printf("A\n");
		}
		// disk info block (block 1)
		// disk verification
		total_read = 56;
		if (memcmp(buffer, bios_string, 15) != 0) {
			printf("FATAL: bios string invalid at offset 0x%X\n", total_read - 56);
			bailout();
		}
		if (dest) {
			fwrite(buffer, 56, 1, fout);
			total_write = 56;
		}
		if (source == QD) {
			if (!fread(crc_buffer, 2, 1, fin)) {
				printf("FATAL: fread failure for block 1 crc\n");
				bailout();
			}
			total_read += 2;
			crc_read = (crc_buffer[1] * 256) + crc_buffer[0];
			crc_calc = gen_qd_crc(buffer, 56);
			// Nintendo sometimes uses a null CRC for the disk info block
			if (crc_read != crc_calc && crc_read != 0x0000) {
				if (verbose)
					printf("  ");
				printf("WARNING: CRC mismatch at offset 0x%X, read 0x%02X%02X, expected 0x%02X%02X\n", total_read - 2, crc_read % 256, crc_read / 256, crc_calc % 256, crc_calc / 256);
				rc = 1;
			}
		}
		if (dest == QD) {
			if (dib_zero) {
				fputc(0, fout);
				fputc(0, fout);
			}
			else {
				crc_calc = gen_qd_crc(buffer, 56);
				fwrite(&crc_calc, 2, 1, fout);
			}
			total_write += 2;
		}

		if (verbose) {
			printf("  Manufacturer: ");
			switch (buffer[15]) {
			case 0x00:
				printf("<unlicensed>");
				break;
			case 0x01:
				printf("Nintendo");
				break;
			case 0x08:
				printf("Capcom");
				break;
			case 0x0A:
				printf("Jaleco");
				break;
			case 0x18:
				printf("Hudson Soft");
				break;
			case 0x49:
				printf("Irem");
				break;
			case 0x4A:
				printf("Gakken");
				break;
			case 0x8B:
				printf("BulletProof Software (BPS)");
				break;
			case 0x99:
				printf("Pack-In-Video");
				break;
			case 0x9B:
				printf("Tecmo");
				break;
			case 0x9C:
				printf("Imagineer");
				break;
			case 0xA2:
				printf("Scorpion Soft");
				break;
			case 0xA4:
				printf("Konami");
				break;
			case 0xA6:
				printf("Kawada Co., Ltd.");
				break;
			case 0xA7:
				printf("Takara");
				break;
			case 0xA8:
				printf("Royal Industries");
				break;
			case 0xAC:
				printf("Toei Animation");
				break;
			case 0xAF:
				printf("Namco");
				break;
			case 0xB1:
				printf("ASCII Corporation");
				break;
			case 0xB2:
				printf("Bandai");
				break;
			case 0xB3:
				printf("Soft Pro Inc.");
				break;
			case 0xB6:
				printf("HAL Laboratory");
				break;
			case 0xBB:
				printf("Sunsoft");
				break;
			case 0xBC:
				printf("Toshiba EMI");
				break;
			case 0xC0:
				printf("Taito");
				break;
			case 0xC1:
				printf("Sunsoft / Ask Co., Ltd.");
				break;
			case 0xC2:
				printf("Kemco");
				break;
			case 0xC3:
				printf("Square");
				break;
			case 0xC4:
				printf("Tokuma Shoten");
				break;
			case 0xC5:
				printf("Data East");
				break;
			case 0xC6:
				printf("Tonkin House/Tokyo Shoseki");
				break;
			case 0xC7:
				printf("East Cube");
				break;
			case 0xCA:
				printf("Konami / Ultra / Palcom");
				break;
			case 0xCB:
				printf("NTVIC / VAP");
				break;
			case 0xCC:
				printf("Use Co., Ltd.");
				break;
			case 0xCE:
				printf("Pony Canyon / FCI");
				break;
			case 0xD1:
				printf("Sofel");
				break;
			case 0xD2:
				printf("Bothtec, Inc.");
				break;
			case 0xDB:
				printf("Hiro Co., Ltd.");
				break;
			case 0xE7:
				printf("Athena");
				break;
			case 0xEB:
				printf("Atlus");
				break;
			default:
				printf("<unknown> (0x%02X)", buffer[15]);
			}
			printf("\n");
			printf("  Game name: ");
			for (x = 0; x < 3; x++) {
				if (buffer[16 + x] < 0x20 || buffer[16 + x] > 0x7E)
					printf("?");
				else
					printf("%c", buffer[16 + x]);
			}
			printf("\n");
			printf("  Game type: ");
			switch (buffer[19]) {
			case 0x20:
				printf("Normal disk");
				break;
			case 0x45:
				printf("Event");
				break;
			case 0x52:
				printf("Reduction in price via advertising");
				break;
			default:
				printf("<unknown> (0x%02X)", buffer[19]);
			}
			printf("\n");
			printf("  Game revision: ");
			if (buffer[20] != 0xFF)
				printf("%d", buffer[20]);
			else
				printf("<unknown>");
			printf("\n");
			printf("  Side number: ");
			if (buffer[21] == 0)
				printf("Side A");
			else
				printf("Side B");
			printf("\n");
			printf("  Disk number: %d\n", buffer[22] + 1);
			printf("  Disk type: ");
			switch (buffer[23]) {
			case 0x00:
				printf("Normal card");
				break;
			case 0x01:
				printf("Card with shutter");
				break;
			default:
				printf("<unknown> (0x%02X)", buffer[23]);
			}
			printf("\n");
			printf("  Boot read file code: %d\n", buffer[25]);
			printf("  Manufacturing date: ");
			print_date(buffer + 31);
			printf("  Country code: ");
			switch (buffer[34]) {
			case 0x49:
				printf("Japan");
				break;
			default:
				printf("<unknown> (0x%02X)", buffer[34]);
				break;
			}
			printf("\n");
			printf("  \"Rewritten disk\" date (speculative): ");
			print_date(buffer + 44);
			printf("  Disk writer serial number: ");
			// BUG: this needs to be fixed, no idea what the format is
			printf("\n");
			//      printf("%.*s\n", 2, buffer+49);a
			// BUG: The below is in BCD format, but is it packed or unpacked?
			printf("  Disk rewrite count: %d\n", buffer[52]);
			printf("  Actual disk side: ");
			if (buffer[53] == 0x00)
				printf("Side A");
			else
				printf("Side B");
			printf("\n");
			printf("  Price: ");
			if (buffer[52] == 0x00)
				// new/original disk
				// known but undocumented codes: 0x00, 0x02, 0x04, 0x05, 0x07, 0x10
				// other new codes (on rewrite?): 0xFF, 0xF7, 0x11, 0x04 (rewrite), 0x03 (rewrite - but with peripherals?)
				switch (buffer[55]) {
				case 0x01:
					printf("3400 yen");
					break;
				case 0x03:
					printf("3400 yen (includes peripherals)");
					break;
				default:
					printf("<unknown> (0x%02X)", buffer[55]);
				}
			else
				// rewritten disk
				switch (buffer[55]) {
				case 0x00:
					printf("500 yen");
					break;
				case 0x01:
					printf("600 yen");
					break;
				default:
					printf("<unknown> (0x%02X)", buffer[55]);
				}
			printf("\n");
		}

		// file amount block (block 2)   
		if (!fread(buffer, 2, 1, fin)) {
			printf("FATAL: fread failure for block 2\n");
			bailout();
		}
		total_read += 2;
		if (buffer[0] != 0x02) {
			printf("FATAL: invalid file amount block at offset 0x%X\n", total_read - 2);
			bailout();
		}
		if (dest) {
			fwrite(buffer, 2, 1, fout);
			total_write += 2;
		}
		file_amount = buffer[1];
		if (verbose)
			printf("  File amount: %d\n", file_amount);

		if (source == QD) {
			if (!fread(crc_buffer, 2, 1, fin)) {
				printf("FATAL: fread failure for block 2 crc\n");
				bailout();
			}
			total_read += 2;
			crc_read = (crc_buffer[1] * 256) + crc_buffer[0];
			crc_calc = gen_qd_crc(buffer, 2);
			if (crc_read != crc_calc) {
				if (verbose)
					printf("  ");
				printf("WARNING: CRC mismatch at offset 0x%X, read 0x%02X%02X, expected 0x%02X%02X\n", total_read - 2, crc_read % 256, crc_read / 256, crc_calc % 256, crc_calc * 256);
				rc = 1;
			}
		}
		if (dest == QD) {
			crc_calc = gen_qd_crc(buffer, 2);
			fwrite(&crc_calc, 2, 1, fout);
			total_write += 2;
		}

		while (fread(buffer, 16, 1, fin)) {
			// file header block (block 3)
			if (buffer[0] != 0x03) {
				fseek(fin, -16L, SEEK_CUR);
				break;
			}
			total_read += 16;
			if (dest) {
				fwrite(buffer, 16, 1, fout);
				total_write += 16;
			}
			file_size = (buffer[14] * 256) + buffer[13];

			if (source == QD) {
				if (!fread(crc_buffer, 2, 1, fin)) {
					printf("FATAL: fread failure for block 3 crc\n");
					bailout();
				}
				total_read += 2;
				crc_read = (crc_buffer[1] * 256) + crc_buffer[0];
				crc_calc = gen_qd_crc(buffer, 16);
				if (crc_read != crc_calc) {
					if (verbose)
						printf("  ");
					printf("WARNING: CRC mismatch at offset 0x%X, read 0x%02X%02X, expected 0x%02X%02X\n", total_read - 2, crc_read % 256, crc_read / 256, crc_calc % 256, crc_calc / 256);
					rc = 1;
				}
			}
			if (dest == QD) {
				crc_calc = gen_qd_crc(buffer, 16);
				fwrite(&crc_calc, 2, 1, fout);
				total_write += 2;
			}

			if (verbose) {
				printf("  File number: %d", buffer[1]);
				if (buffer[1] >= file_amount)
					printf(" (hidden)");
				printf("\n");
				printf("    File indicate code: %d\n", buffer[2]);
				printf("    File name: ");
				for (x = 0; x < 3; x++) {
					if (buffer[3 + x] < 0x20 || buffer[3 + x] > 0x7E)
						printf("?");
					else
						printf("%c", buffer[3 + x]);
				}
				printf("\n");
				printf("    File address: $%04X\n", (buffer[12] * 256) + buffer[11]);
				printf("    File size: %d bytes\n", file_size);
				printf("    File kind: ");
				switch (buffer[15]) {
				case 0:
					printf("Program (PRAM)");
					break;
				case 1:
					printf("Character (CRAM)");
					break;
				case 2:
					printf("Name table (VRAM)");
					break;
				default:
					printf("<unknown> (0x%02X)", buffer[15]);
				}
				printf("\n");
			}

			// file data block (block 4)
			if (!fread(buffer, 1 + file_size, 1, fin)) {
				printf("FATAL: fread failure for block 4\n");
				bailout();
			}
			total_read += 1 + file_size;
			if (buffer[0] != 0x04) {
				printf("FATAL: invalid file data block at offset 0x%X\n", total_read - (1 + file_size));
				bailout();
			}
			if (dest) {
				fwrite(buffer, 1 + file_size, 1, fout);
				total_write += 1 + file_size;
			}

			if (source == QD) {
				if (!fread(crc_buffer, 2, 1, fin)) {
					printf("FATAL: fread failure for block 4 crc\n");
					bailout();
				}
				total_read += 2;
				crc_read = (crc_buffer[1] * 256) + crc_buffer[0];
				crc_calc = gen_qd_crc(buffer, 1 + file_size);
				if (crc_read != crc_calc) {
					if (verbose)
						printf("    ");
					printf("WARNING: CRC mismatch at offset 0x%X, read 0x%02X%02X, expected 0x%02X%02X\n", total_read - 2, crc_read % 256, crc_read / 256, crc_calc % 256, crc_calc / 256);
					rc = 1;
				}
			}
			if (dest == QD) {
				crc_calc = gen_qd_crc(buffer, 1 + file_size);
				fwrite(&crc_calc, 2, 1, fout);
				total_write += 2;
			}
		}

		if (source == QD) {
			if (!fread(buffer, 65536 - total_read, 1, fin)) { // read to end
				printf("FATAL: fread failure for end of side\n");
				bailout();
			}
			if (dest) {
				for (x = 0; x < 65500 - total_write; x++)
					fputc(0, fout);
				if (!remove_header && !header_sides) {
					fseek(fout, 4L, SEEK_SET);
					fputc(total_sides, fout);
				}
			}
		}
		else {
			if (!fread(buffer, 65500 - total_read, 1, fin)) { // read to end
				printf("FATAL: fread failure for end of side\n");
				bailout();
			}
			if (dest)
				if (dest == QD)
					for (x = 0; x < 65536 - total_write; x++)
						fputc(0, fout);
				else
					for (x = 0; x < 65500 - total_write; x++)
						fputc(0, fout);
		}
	}

	if (header_sides && (header_sides != total_sides)) {
		printf("WARNING: fds header sides mismatch (header: %d, file: %d)\n", header_sides, total_sides);
		rc = 1;
	}

	fclose(fin);
	if (fout)
		fclose(fout);

	exit(rc);
}
