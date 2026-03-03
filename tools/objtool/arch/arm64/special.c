// SPDX-License-Identifier: GPL-2.0-or-later
#include <objtool/special.h>

/*
 * On arm64 a zero-length replacement means the alternative has a callback,
 * whose address is stored in the replacement offset.  It must be preserved.
 */
bool arch_alt_ignore_new_reloc(struct section *sec, unsigned long offset)
{
	return false;
}

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	return true;
}

struct reloc *arch_find_switch_table(struct objtool_file *file,
				     struct instruction *insn,
				     unsigned long *table_size)
{
	return NULL;
}

const char *arch_cpu_feature_name(int feature_number)
{
	return NULL;
}
