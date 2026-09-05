/*
 * assembler.c
 *
 * Two-pass assembler for the .pvms text format (see example/bytecode_listing.pvms):
 *   - '#' line comments
 *   - ".import file"  : textually inlines another .pvms file (recursively, cycle-safe)
 *   - ".const NAME V" : named integer constant
 *   - ".entry label"  : moves the block starting at `label` to address 0
 *   - "name:"         : label starting a new code block
 *   - "MNEMONIC [operand]" : one instruction (operand is a number, .const name or label)
 */
#include "assembler.h"
#include "pvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <stdlib.h>
#define PVMC_FULLPATH(rel, abs, size) _fullpath((abs), (rel), (size))
#else
#include <limits.h>
#define PVMC_FULLPATH(rel, abs, size) realpath((rel), (abs))
#endif

#define MAX_NAME_LEN	64
#define MAX_LINE_LEN	1024
#define MAX_PATH_LEN	1024

typedef struct {
	const char* name;
	int 				opcode;
	int 				has_operand;
} OpcodeDef_s;

static const OpcodeDef_s k_opcodes[] = {
	{"PUSH", 		INSTR_PUSH, 	1},
	{"POP", 		INSTR_POP, 		0},
	{"SWAP", 		INSTR_SWAP, 	0},
	{"DUP", 		INSTR_DUP, 		0},
	{"ADD", 		INSTR_ADD, 		0},
	{"SUB", 		INSTR_SUB, 		0},
	{"MUL", 		INSTR_MUL, 		0},
	{"DIV", 		INSTR_DIV, 		0},
	{"EQ", 			INSTR_EQ, 		0},
	{"LT", 			INSTR_LT, 		0},
	{"JMP", 		INSTR_JMP, 		1},
	{"JZ", 			INSTR_JZ, 		1},
	{"JNZ", 		INSTR_JNZ, 		1},
	{"LOAD", 		INSTR_LOAD, 	1},
	{"STORE", 	INSTR_STORE, 	1},
	{"CALL", 		INSTR_CALL, 	1},
	{"RET", 		INSTR_RET, 		0},
	{"LOADI", 	INSTR_LOADI, 	0},
	{"STOREI", 	INSTR_STOREI, 0},
	{"HALT", 		INSTR_HALT, 	0},
	{"AND", 		INSTR_AND, 		0},
	{"OR", 			INSTR_OR, 		0},
	{"XOR", 		INSTR_XOR, 		0},
	{"SHL", 		INSTR_SHL, 		0},
	{"SHR", 		INSTR_SHR, 		0},
	{"NOT", 		INSTR_NOT, 		0},
};
#define NUM_OPCODES (int)(sizeof(k_opcodes) / sizeof(k_opcodes[0]))

typedef struct {
	char* text;
	char* file;
	int 	line_no;
} Line_s;

typedef struct {
	Line_s* items;
	int 		count;
	int 		capacity;
} LineList_s;

typedef struct {
	char** items;
	int 	 count;
	int 	 capacity;
} PathStack_s;

typedef struct {
	char name[MAX_NAME_LEN];
	int 	value;
} Const_s;

typedef struct {
	Const_s* items;
	int 		 count;
	int 		 capacity;
} ConstList_s;

typedef struct {
	int 	opcode;
	int 	has_operand;
	char 	operand[MAX_NAME_LEN];
	int 	address;
	char* file;
	int 	line_no;
} Instr_s;

typedef struct {
	char 		 label[MAX_NAME_LEN];
	Instr_s* instrs;
	int 		 count;
	int 		 capacity;
	int 		 address;
} Block_s;

typedef struct {
	Block_s* items;
	int 		 count;
	int 		 capacity;
} BlockList_s;

static char* xstrdup(const char* s){
	size_t len = strlen(s) + 1;
	char* out = malloc(len);
	memcpy(out, s, len);
	return out;
}

static void Trim(char* s){
	int len = (int)strlen(s);
	int start = 0;

	while(len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
	while(s[start] && isspace((unsigned char)s[start])) start++;
	if(start > 0) memmove(s, s + start, len - start + 1);
}

static void LineList_Push(LineList_s* list, const char* text, const char* file, int line_no){
	if(list->count == list->capacity){
		list->capacity = list->capacity ? list->capacity * 2 : 64;
		list->items = realloc(list->items, list->capacity * sizeof(Line_s));
	}

	list->items[list->count].text 		= xstrdup(text);
	list->items[list->count].file 		= xstrdup(file);
	list->items[list->count].line_no = line_no;
	list->count++;
}

static void PathStack_Push(PathStack_s* stack, const char* path){
	if(stack->count == stack->capacity){
		stack->capacity = stack->capacity ? stack->capacity * 2 : 8;
		stack->items = realloc(stack->items, stack->capacity * sizeof(char*));
	}
	stack->items[stack->count++] = xstrdup(path);
}

static void PathStack_Pop(PathStack_s* stack){
	stack->count--;
	free(stack->items[stack->count]);
}

static int PathStack_Contains(PathStack_s* stack, const char* path){
	int i;
	for(i = 0; i < stack->count; i++)
		if(strcmp(stack->items[i], path) == 0) return 1;
	return 0;
}

static void SplitDir(const char* path, char* dir_out, int dir_out_size){
	const char* slash1 = strrchr(path, '/');
	const char* slash2 = strrchr(path, '\\');
	const char* slash 	= (slash1 > slash2) ? slash1 : slash2;

	if(slash){
		int len = (int)(slash - path + 1);
		if(len >= dir_out_size) len = dir_out_size - 1;
		memcpy(dir_out, path, len);
		dir_out[len] = 0;
	}
	else
		dir_out[0] = 0;
}

static int FileExists(const char* path){
	FILE* f = fopen(path, "rb");
	if(f){ fclose(f); return 1; }
	return 0;
}

static void ResolveImportPath(const char* importer_path, const char* import_name, char* out, int out_size){
	char dir[MAX_PATH_LEN];
	char candidate[MAX_PATH_LEN];

	SplitDir(importer_path, dir, sizeof(dir));

	snprintf(candidate, sizeof(candidate), "%s%s", dir, import_name);
	if(FileExists(candidate)){
		snprintf(out, out_size, "%s", candidate);
		return;
	}

	snprintf(out, out_size, "%s%s.pvms", dir, import_name);
}

static const OpcodeDef_s* FindOpcode(const char* name){
	int i;
	for(i = 0; i < NUM_OPCODES; i++)
		if(strcmp(k_opcodes[i].name, name) == 0) return &k_opcodes[i];
	return 0;
}

static int ParseNumber(const char* s, int* out){
	char* endptr;
	long v = strtol(s, &endptr, 0);
	if(*s == 0 || *endptr != 0) return 0;
	*out = (int)v;
	return 1;
}

static void ConstList_Add(ConstList_s* list, const char* name, int value){
	if(list->count == list->capacity){
		list->capacity = list->capacity ? list->capacity * 2 : 16;
		list->items = realloc(list->items, list->capacity * sizeof(Const_s));
	}
	snprintf(list->items[list->count].name, MAX_NAME_LEN, "%s", name);
	list->items[list->count].value = value;
	list->count++;
}

static int ConstList_Find(ConstList_s* list, const char* name, int* out_value){
	int i;
	for(i = 0; i < list->count; i++)
		if(strcmp(list->items[i].name, name) == 0){ *out_value = list->items[i].value; return 1; }
	return 0;
}

static Block_s* BlockList_NewBlock(BlockList_s* list, const char* label){
	Block_s* b;

	if(list->count == list->capacity){
		list->capacity = list->capacity ? list->capacity * 2 : 16;
		list->items = realloc(list->items, list->capacity * sizeof(Block_s));
	}
	b = &list->items[list->count++];
	memset(b, 0, sizeof(*b));
	snprintf(b->label, MAX_NAME_LEN, "%s", label ? label : "");
	return b;
}

static void Block_AddInstr(Block_s* b, int opcode, int has_operand, const char* operand, const char* file, int line_no){
	Instr_s* ins;

	if(b->count == b->capacity){
		b->capacity = b->capacity ? b->capacity * 2 : 16;
		b->instrs = realloc(b->instrs, b->capacity * sizeof(Instr_s));
	}
	ins = &b->instrs[b->count++];
	ins->opcode 			= opcode;
	ins->has_operand 	= has_operand;
	snprintf(ins->operand, MAX_NAME_LEN, "%s", operand ? operand : "");
	ins->file 				= xstrdup(file);
	ins->line_no 			= line_no;
}

static int FindLabelAddress(BlockList_s* blocks, const char* name, int* out_addr){
	int i;
	for(i = 0; i < blocks->count; i++)
		if(blocks->items[i].label[0] && strcmp(blocks->items[i].label, name) == 0){
			*out_addr = blocks->items[i].address;
			return 1;
		}
	return 0;
}

static int ResolveOperand(const char* text, ConstList_s* consts, BlockList_s* blocks, int* out_value){
	if(ConstList_Find(consts, text, out_value)) return 1;
	if(FindLabelAddress(blocks, text, out_value)) return 1;
	if(ParseNumber(text, out_value)) return 1;
	return 0;
}

static int LoadFileLines(const char* path, LineList_s* out, PathStack_s* stack){
	char norm[MAX_PATH_LEN];
	FILE* f;
	char buf[MAX_LINE_LEN];
	int line_no = 0;
	int status = 0;

	if(!PVMC_FULLPATH(path, norm, sizeof(norm)))
		snprintf(norm, sizeof(norm), "%s", path);

	if(PathStack_Contains(stack, norm)){
		fprintf(stderr, "pvmc: error: circular .import involving '%s'\n", path);
		return -1;
	}

	f = fopen(path, "rb");
	if(!f){
		fprintf(stderr, "pvmc: error: cannot open '%s'\n", path);
		return -1;
	}
	PathStack_Push(stack, norm);

	while(fgets(buf, sizeof(buf), f)){
		char* hash;

		line_no++;
		hash = strchr(buf, '#');
		if(hash) *hash = 0;
		Trim(buf);
		if(buf[0] == 0) continue;

		if(strncmp(buf, ".import", 7) == 0 && (buf[7] == 0 || isspace((unsigned char)buf[7]))){
			char name[MAX_NAME_LEN] = {0};
			char resolved[MAX_PATH_LEN];

			sscanf(buf + 7, "%63s", name);
			ResolveImportPath(path, name, resolved, sizeof(resolved));

			status = LoadFileLines(resolved, out, stack);
			if(status != 0) break;
			continue;
		}

		LineList_Push(out, buf, path, line_no);
	}

	fclose(f);
	PathStack_Pop(stack);
	return status;
}

static int IsLabelLine(const char* line, char* label_out, int label_out_size){
	int len = (int)strlen(line);
	int i;

	if(len < 2 || line[len - 1] != ':') return 0;
	for(i = 0; i < len - 1; i++)
		if(!(isalnum((unsigned char)line[i]) || line[i] == '_')) return 0;

	if(len - 1 >= label_out_size) return 0;
	memcpy(label_out, line, len - 1);
	label_out[len - 1] = 0;
	return 1;
}

int PVMC_Assemble(const char* input_path, uint8_t** out_code, int* out_len){
	LineList_s 	lines 	= {0};
	PathStack_s stack 	= {0};
	ConstList_s consts 	= {0};
	BlockList_s blocks 	= {0};
	Block_s* 		current = 0;
	char 				entry_label[MAX_NAME_LEN] = {0};
	int 				have_entry = 0;
	int 				running = 0;
	uint8_t* 		code;
	int 				i, j;

	if(LoadFileLines(input_path, &lines, &stack) != 0)
		return -1;

	for(i = 0; i < lines.count; i++){
		const char* line = lines.items[i].text;
		const char* file = lines.items[i].file;
		int line_no 		 = lines.items[i].line_no;
		char label[MAX_NAME_LEN];

		if(strncmp(line, ".entry", 6) == 0 && (line[6] == 0 || isspace((unsigned char)line[6]))){
			char name[MAX_NAME_LEN] = {0};
			sscanf(line + 6, "%63s", name);
			snprintf(entry_label, sizeof(entry_label), "%s", name);
			have_entry = 1;
			continue;
		}

		if(strncmp(line, ".const", 6) == 0 && (line[6] == 0 || isspace((unsigned char)line[6]))){
			char name[MAX_NAME_LEN] = {0}, valtext[MAX_NAME_LEN] = {0};
			int value;

			if(sscanf(line + 6, "%63s %63s", name, valtext) != 2){
				fprintf(stderr, "%s:%d: error: malformed .const directive\n", file, line_no);
				return -1;
			}
			if(!ParseNumber(valtext, &value)){
				fprintf(stderr, "%s:%d: error: invalid constant value '%s'\n", file, line_no, valtext);
				return -1;
			}
			ConstList_Add(&consts, name, value);
			continue;
		}

		if(IsLabelLine(line, label, sizeof(label))){
			current = BlockList_NewBlock(&blocks, label);
			continue;
		}

		{
			char mnemonic[16] = {0}, operand[MAX_NAME_LEN] = {0};
			int n = sscanf(line, "%15s %63s", mnemonic, operand);
			const OpcodeDef_s* def = FindOpcode(mnemonic);

			if(!def){
				fprintf(stderr, "%s:%d: error: unknown instruction '%s'\n", file, line_no, mnemonic);
				return -1;
			}
			if(def->has_operand && n < 2){
				fprintf(stderr, "%s:%d: error: '%s' requires an operand\n", file, line_no, mnemonic);
				return -1;
			}
			if(!def->has_operand && n > 1){
				fprintf(stderr, "%s:%d: error: '%s' takes no operand\n", file, line_no, mnemonic);
				return -1;
			}

			if(!current)
				current = BlockList_NewBlock(&blocks, "");

			Block_AddInstr(current, def->opcode, def->has_operand, operand, file, line_no);
		}
	}

	if(have_entry){
		/*
		 * .entry doesn't just relocate the single labeled block: everything from the
		 * entry label to end-of-file is one contiguous unit (it may contain further
		 * labels that are just local jump targets, e.g. a loop inside main). So this
		 * is a rotation of the whole block sequence to start at the entry block, not
		 * a "move one block, keep the rest in place" operation.
		 */
		int idx = -1;
		Block_s* reordered;
		int k = 0;

		for(i = 0; i < blocks.count; i++)
			if(strcmp(blocks.items[i].label, entry_label) == 0){ idx = i; break; }

		if(idx < 0){
			fprintf(stderr, "pvmc: error: .entry label '%s' not found\n", entry_label);
			return -1;
		}

		reordered = malloc(blocks.count * sizeof(Block_s));
		for(i = idx; i < blocks.count; i++) reordered[k++] = blocks.items[i];
		for(i = 0; i < idx; i++) reordered[k++] = blocks.items[i];

		free(blocks.items);
		blocks.items = reordered;
	}

	for(i = 0; i < blocks.count; i++){
		Block_s* b = &blocks.items[i];
		b->address = running;
		for(j = 0; j < b->count; j++){
			Instr_s* ins = &b->instrs[j];
			ins->address = running;
			running += ins->has_operand ? 3 : 1;
		}
	}

	code = calloc(running > 0 ? running : 1, 1);
	for(i = 0; i < blocks.count; i++){
		Block_s* b = &blocks.items[i];
		for(j = 0; j < b->count; j++){
			Instr_s* ins = &b->instrs[j];
			code[ins->address] = (uint8_t)ins->opcode;

			if(ins->has_operand){
				int value;
				if(!ResolveOperand(ins->operand, &consts, &blocks, &value)){
					fprintf(stderr, "%s:%d: error: undefined symbol '%s'\n", ins->file, ins->line_no, ins->operand);
					free(code);
					return -1;
				}
				code[ins->address + 1] = (uint8_t)(value & 0xFF);
				code[ins->address + 2] = (uint8_t)((value >> 8) & 0xFF);
			}
		}
	}

	*out_code = code;
	*out_len  = running;
	return 0;
}
