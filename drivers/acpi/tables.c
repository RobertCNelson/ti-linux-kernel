/*
 *  acpi_tables.c - ACPI Boot-Time Table Parsing
 *
 *  Copyright (C) 2001 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */

/* Uncomment next line to get verbose printout */
/* #define DEBUG */
#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/bootmem.h>

#define ACPI_MAX_TABLES		128

static char *mps_inti_flags_polarity[] = { "dfl", "high", "res", "low" };
static char *mps_inti_flags_trigger[] = { "dfl", "edge", "res", "level" };

static struct acpi_table_desc initial_tables[ACPI_MAX_TABLES] __initdata;

static int acpi_apic_instance __initdata;

/*
 * Disable table checksum verification for the early stage due to the size
 * limitation of the current x86 early mapping implementation.
 */
static bool acpi_verify_table_checksum __initdata = false;

void acpi_table_print_madt_entry(struct acpi_subtable_header *header)
{
	if (!header)
		return;

	switch (header->type) {

	case ACPI_MADT_TYPE_LOCAL_APIC:
		{
			struct acpi_madt_local_apic *p =
			    (struct acpi_madt_local_apic *)header;
			pr_debug("LAPIC (acpi_id[0x%02x] lapic_id[0x%02x] %s)\n",
				 p->processor_id, p->id,
				 (p->lapic_flags & ACPI_MADT_ENABLED) ? "enabled" : "disabled");
		}
		break;

	case ACPI_MADT_TYPE_LOCAL_X2APIC:
		{
			struct acpi_madt_local_x2apic *p =
			    (struct acpi_madt_local_x2apic *)header;
			pr_debug("X2APIC (apic_id[0x%02x] uid[0x%02x] %s)\n",
				 p->local_apic_id, p->uid,
				 (p->lapic_flags & ACPI_MADT_ENABLED) ? "enabled" : "disabled");
		}
		break;

	case ACPI_MADT_TYPE_IO_APIC:
		{
			struct acpi_madt_io_apic *p =
			    (struct acpi_madt_io_apic *)header;
			pr_debug("IOAPIC (id[0x%02x] address[0x%08x] gsi_base[%d])\n",
				 p->id, p->address, p->global_irq_base);
		}
		break;

	case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
		{
			struct acpi_madt_interrupt_override *p =
			    (struct acpi_madt_interrupt_override *)header;
			pr_info("INT_SRC_OVR (bus %d bus_irq %d global_irq %d %s %s)\n",
				p->bus, p->source_irq, p->global_irq,
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2]);
			if (p->inti_flags  &
			    ~(ACPI_MADT_POLARITY_MASK | ACPI_MADT_TRIGGER_MASK))
				pr_info("INT_SRC_OVR unexpected reserved flags: 0x%x\n",
					p->inti_flags  &
					~(ACPI_MADT_POLARITY_MASK | ACPI_MADT_TRIGGER_MASK));
		}
		break;

	case ACPI_MADT_TYPE_NMI_SOURCE:
		{
			struct acpi_madt_nmi_source *p =
			    (struct acpi_madt_nmi_source *)header;
			pr_info("NMI_SRC (%s %s global_irq %d)\n",
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->global_irq);
		}
		break;

	case ACPI_MADT_TYPE_LOCAL_APIC_NMI:
		{
			struct acpi_madt_local_apic_nmi *p =
			    (struct acpi_madt_local_apic_nmi *)header;
			pr_info("LAPIC_NMI (acpi_id[0x%02x] %s %s lint[0x%x])\n",
				p->processor_id,
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK	],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->lint);
		}
		break;

	case ACPI_MADT_TYPE_LOCAL_X2APIC_NMI:
		{
			u16 polarity, trigger;
			struct acpi_madt_local_x2apic_nmi *p =
			    (struct acpi_madt_local_x2apic_nmi *)header;

			polarity = p->inti_flags & ACPI_MADT_POLARITY_MASK;
			trigger = (p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2;

			pr_info("X2APIC_NMI (uid[0x%02x] %s %s lint[0x%x])\n",
				p->uid,
				mps_inti_flags_polarity[polarity],
				mps_inti_flags_trigger[trigger],
				p->lint);
		}
		break;

	case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE:
		{
			struct acpi_madt_local_apic_override *p =
			    (struct acpi_madt_local_apic_override *)header;
			pr_info("LAPIC_ADDR_OVR (address[%p])\n",
				(void *)(unsigned long)p->address);
		}
		break;

	case ACPI_MADT_TYPE_IO_SAPIC:
		{
			struct acpi_madt_io_sapic *p =
			    (struct acpi_madt_io_sapic *)header;
			pr_debug("IOSAPIC (id[0x%x] address[%p] gsi_base[%d])\n",
				 p->id, (void *)(unsigned long)p->address,
				 p->global_irq_base);
		}
		break;

	case ACPI_MADT_TYPE_LOCAL_SAPIC:
		{
			struct acpi_madt_local_sapic *p =
			    (struct acpi_madt_local_sapic *)header;
			pr_debug("LSAPIC (acpi_id[0x%02x] lsapic_id[0x%02x] lsapic_eid[0x%02x] %s)\n",
				 p->processor_id, p->id, p->eid,
				 (p->lapic_flags & ACPI_MADT_ENABLED) ? "enabled" : "disabled");
		}
		break;

	case ACPI_MADT_TYPE_INTERRUPT_SOURCE:
		{
			struct acpi_madt_interrupt_source *p =
			    (struct acpi_madt_interrupt_source *)header;
			pr_info("PLAT_INT_SRC (%s %s type[0x%x] id[0x%04x] eid[0x%x] iosapic_vector[0x%x] global_irq[0x%x]\n",
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->type, p->id, p->eid, p->io_sapic_vector,
				p->global_irq);
		}
		break;

	case ACPI_MADT_TYPE_GENERIC_INTERRUPT:
		{
			struct acpi_madt_generic_interrupt *p =
				(struct acpi_madt_generic_interrupt *)header;
			pr_debug("GICC (acpi_id[0x%04x] address[%llx] MPIDR[0x%llx] %s)\n",
				 p->uid, p->base_address,
				 p->arm_mpidr,
				 (p->flags & ACPI_MADT_ENABLED) ? "enabled" : "disabled");

		}
		break;

	case ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR:
		{
			struct acpi_madt_generic_distributor *p =
				(struct acpi_madt_generic_distributor *)header;
			pr_debug("GIC Distributor (gic_id[0x%04x] address[%llx] gsi_base[%d])\n",
				 p->gic_id, p->base_address,
				 p->global_irq_base);
		}
		break;

	default:
		pr_warn("Found unsupported MADT entry (type = 0x%x)\n",
			header->type);
		break;
	}
}

/*
 * The Long, Sad, True Story of the MADT
 *    or
 * What does bad_madt_entry() actually do?
 *
 * Once upon a time in ACPI 1.0, there was the MADT.  It was a nice table,
 * and it had two subtables all of its own.  But, it was also a pretty
 * busy table, too, so over time the MADT gathered up other nice little
 * subtables.  By the time ACPI 6.0 came around, the MADT had 16 of the
 * little guys.
 *
 * Now, the MADT kept a little counter around for the subtables.  In fact,
 * it kept two counters: one was the revision level, which was supposed to
 * change when new subtables came to be, or as the ones already around grew
 * up. The second counter was a type number, because the MADT needed a unique
 * type for each subtable so he could tell them apart.  But, sometimes the
 * MADT got so busy, he forgot to increment the revision level when he needed
 * to.  Fortunately, the type counter kept increasing since that's the only
 * way the MADT could find each little subtable.  It just wouldn't do to have
 * every subtable called Number 6.
 *
 * In the next valley over, a castle full of wizards was watching the MADT
 * and made a pact to keep their own counter.  Every time the MADT found a
 * new subtable, or a subtable grew up, the wizards promised they would
 * increment their counter.  Well, wizards being the forgetful sort, they
 * didn't alway do that.  And, since there quite a lot of them, they
 * couldn't always remember who was supposed to keep track of the MADT,
 * especially if dinner was coming up soon.  Their counter was called the
 * spec version.
 *
 * Every now and then, the MADT would gather up all its little subtables
 * and take them in to the cobbler to get new boots.  This was a very, very
 * meticulous cobbler, so every time they came, he wrote down all the boot
 * sizes for all of the little subtables.  The cobbler would ask each subtable
 * for its length, check that against his careful notes, and then go get the
 * right boots.  Sometimes, a little subtable would change a bit, and their
 * length did not match what the cobbler had written down.  If the wizards
 * or the MADT had incremented their counters, the cobbler would breath a
 * sigh of relief and write down the new length as the right one.  But, if
 * none of the counters had changed, this would make the cobbler very, very
 * mad.  He couldn't tell if he had the right size boots or not for the
 * little subtable.  He would have to *guess* and this really bugged him.
 *
 * Well, when the cobbler got mad like this, he would go into hiding.  He
 * would not make or sell any boots.  He would not go out at all.  Pretty
 * soon, the coffee shop would have to close because the cobbler wasn't
 * coming by twice a day any more.  Then the grocery store would have to
 * close because he wouldn't eat much.  After a while, everyone would panic
 * and have to move from the village and go live with all their relatives
 * (usually the ones they didn't like very much).
 *
 * Eventually, the cobbler would work his way out of his bad mood, and
 * open up his boot business again.  Then, everyone else could move back
 * to the village and restart their lives, too.
 *
 * Fortunately, we have been able to collect up all the cobbler's careful
 * notes (and we wrote them down below).  We'll have to keep checking these
 * notes over time, too, just as the cobbler does.  But, in the meantime,
 * we can avoid the panic and the reboot since we can make sure that each
 * subtable is doing okay.  And that's what bad_madt_entry() does.
 *
 *
 * FADT Major Version ->       1    3    4     4     5     5     6
 * FADT Minor Version ->       x    x    x     x     x     1     0
 * MADT revision ->            1    1    2     3     3     3     3
 * Spec Version ->            1.0  2.0  3.0b  4.0a  5.0b  5.1a  6.0
 * Subtable Name	Type  Expected Length ->
 * Processor Local APIC  0x0    8    8    8     8     8     8     8
 * IO APIC               0x1   12   12   12    12    12    12    12
 * Int Src Override      0x2   10   10   10    10    10    10    10
 * NMI Src               0x3    8    8    8     8     8     8     8
 * Local APIC NMI Struct 0x4    6    6    6     6     6     6     6
 * Local APIC Addr Ovrrd 0x5        16   12    12    12    12    12
 * IO SAPIC              0x6        20   16    16    16    16    16
 * Local SAPIC           0x7         8  >16   >16   >16   >16   >16
 * Platform Int Src      0x8        16   16    16    16    16    16
 * Proc Local x2APIC     0x9                   16    16    16    16
 * Local x2APIC NMI      0xa                   12    12    12    12
 * GICC CPU I/F          0xb                         40    76    80
 * GICD                  0xc                         24    24    24
 * GICv2m MSI            0xd                               24    24
 * GICR                  0xe                               16    16
 * GIC ITS               0xf                                     16
 *
 * In the table, each length entry is what should be in the length
 * field of the subtable, and -- in general -- it should match the
 * size of the struct for the subtable.  Any value that is not set
 * (i.e., is zero) indicates that the subtable is not defined for
 * that version of the ACPI spec.
 *
 */
#define SUBTABLE_UNDEFINED	0x00
#define SUBTABLE_VARIABLE	0xff
#define NUM_SUBTABLE_TYPES	16

struct acpi_madt_subtable_lengths {
	unsigned short major_version;	/* from revision in FADT header */
	unsigned short minor_version;	/* FADT field starting with 5.1 */
	unsigned short madt_version;	/* MADT revision */
	unsigned short num_types;	/* types possible for this version */
	unsigned short lengths[NUM_SUBTABLE_TYPES];
					/* subtable lengths, indexed by type */
};

static struct acpi_madt_subtable_lengths spec_info[] = {
	{ /* for ACPI 1.0b */
		.major_version = 1,
		.minor_version = 0,
		.madt_version = 1,
		.num_types = 5,
		.lengths = { 8, 12, 10, 8, 6 }
	},
	{ /* for ACPI 2.0 */
		.major_version = 3,
		.minor_version = 0,
		.madt_version = 1,
		.num_types = 9,
		.lengths = { 8, 12, 10, 8, 6, 16, 20, 8, 16 }
	},
	{ /* for ACPI 3.0b */
		.major_version = 4,
		.minor_version = 0,
		.madt_version = 2,
		.num_types = 9,
		.lengths = { 8, 12, 10, 8, 6, 12, 16, SUBTABLE_VARIABLE, 16 }
	},
	{ /* for ACPI 4.0a */
		.major_version = 4,
		.minor_version = 0,
		.madt_version = 3,
		.num_types = 11,
		.lengths = { 8, 12, 10, 8, 6, 12, 16, SUBTABLE_VARIABLE,
			     16, 16, 12 }
	},
	{ /* for ACPI 5.0b */
		.major_version = 5,
		.minor_version = 0,
		.madt_version = 3,
		.num_types = 13,
		.lengths = { 8, 12, 10, 8, 6, 12, 16, SUBTABLE_VARIABLE,
			     16, 16, 12, 40, 24 }
	},
	{ /* for ACPI 5.1a */
		.major_version = 5,
		.minor_version = 1,
		.madt_version = 3,
		.num_types = 15,
		.lengths = { 8, 12, 10, 8, 6, 12, 16, SUBTABLE_VARIABLE,
			     16, 16, 12, 76, 24, 24, 16 }
	},
	{ /* for ACPI 6.0 */
		.major_version = 6,
		.minor_version = 0,
		.madt_version = 3,
		.num_types = 16,
		.lengths = { 8, 12, 10, 8, 6, 12, 16, SUBTABLE_VARIABLE,
			     16, 16, 12, 80, 24, 24, 16, 16 }
	},
	{ /* terminator */
		.major_version = 0,
		.minor_version = 0,
		.madt_version = 0,
		.num_types = 0,
		.lengths = { 0 }
	}
};

static int __init bad_madt_entry(struct acpi_table_header *table,
				 struct acpi_subtable_header *entry)
{
	struct acpi_madt_subtable_lengths *ms;
	struct acpi_table_madt *madt;
	unsigned short major;
	unsigned short minor;
	unsigned short len;

	/* simple sanity checking on MADT subtable entries */
	if (!entry || !table)
		return 1;

	/* FADT minor numbers were not introduced until ACPI 5.1 */
	major = acpi_gbl_FADT.header.revision;
	if (major >= 5 && acpi_gbl_FADT.header.length >= 268)
		minor = acpi_gbl_FADT.minor_revision;
	else
		minor = 0;

	madt = (struct acpi_table_madt *)table;
	ms = spec_info;
	while (ms->num_types != 0) {
		if (ms->major_version == major &&
		    ms->minor_version == minor &&
		    ms->madt_version == madt->header.revision)
			break;
		ms++;
	}
	if (!ms->num_types) {
		pr_err("undefined version for either FADT %d.%d or MADT %d\n",
		       major, minor, madt->header.revision);
		return 1;
	}

	if (entry->type >= ms->num_types) {
		pr_err("undefined MADT subtable type for FADT %d.%d: %d (length %d)\n",
		       major, minor, entry->type, entry->length);
		return 1;
	}

	/* verify that the table is allowed for this version of the spec */
	len = ms->lengths[entry->type];
	if (!len) {
		pr_err("MADT subtable %d not defined for FADT %d.%d\n",
			 entry->type, major, minor);
		return 1;
	}

	/* verify that the length is what we expect */
	if (len == SUBTABLE_VARIABLE) {
		if (entry->type == ACPI_MADT_TYPE_LOCAL_SAPIC) {
			struct acpi_madt_local_sapic *lsapic =
				(struct acpi_madt_local_sapic *)entry;
			int proper_len = sizeof(struct acpi_madt_local_sapic) +
					 strlen(lsapic->uid_string) + 1;

			if (proper_len != entry->length) {
				pr_err("Variable length MADT subtable %d is wrong length: %d, should be: %d\n",
				       entry->type, entry->length, proper_len);
				return 1;
			}
		}
	} else {
		if (entry->length != len) {
			pr_err("MADT subtable %d is wrong length: %d, should be: %d\n",
			       entry->type, entry->length, len);
			return 1;
		}
	}

	return 0;
}

/**
 * acpi_parse_entries_array - for each proc_num find a suitable subtable
 *
 * @id: table id (for debugging purposes)
 * @table_size: single entry size
 * @table_header: where does the table start?
 * @proc: array of acpi_subtable_proc struct containing entry id
 *        and associated handler with it
 * @proc_num: how big proc is?
 * @max_entries: how many entries can we process?
 *
 * For each proc_num find a subtable with proc->id and run proc->handler
 * on it. Assumption is that there's only single handler for particular
 * entry id.
 *
 * On success returns sum of all matching entries for all proc handlers.
 * Otherwise, -ENODEV or -EINVAL is returned.
 */
static int __init
acpi_parse_entries_array(char *id, unsigned long table_size,
		struct acpi_table_header *table_header,
		struct acpi_subtable_proc *proc, int proc_num,
		unsigned int max_entries)
{
	struct acpi_subtable_header *entry;
	unsigned long table_end;
	int count = 0;
	int i;

	if (acpi_disabled)
		return -ENODEV;

	if (!id)
		return -EINVAL;

	if (!table_size)
		return -EINVAL;

	if (!table_header) {
		pr_warn("%4.4s not present\n", id);
		return -ENODEV;
	}

	table_end = (unsigned long)table_header + table_header->length;

	/* Parse all entries looking for a match. */

	entry = (struct acpi_subtable_header *)
	    ((unsigned long)table_header + table_size);

	while (((unsigned long)entry) + sizeof(struct acpi_subtable_header) <
	       table_end) {
		if (max_entries && count >= max_entries)
			break;

		if (!strncmp(id, ACPI_SIG_MADT, 4) &&
		    bad_madt_entry(table_header, entry))
			return -EINVAL;

		for (i = 0; i < proc_num; i++) {
			if (entry->type != proc[i].id)
				continue;
			if (!proc[i].handler ||
			     proc[i].handler(entry, table_end))
				return -EINVAL;

			proc->count++;
			break;
		}
		if (i != proc_num)
			count++;

		/*
		 * If entry->length is 0, break from this loop to avoid
		 * infinite loop.
		 */
		if (entry->length == 0) {
			pr_err("[%4.4s:0x%02x] Invalid zero length\n", id, proc->id);
			return -EINVAL;
		}

		entry = (struct acpi_subtable_header *)
		    ((unsigned long)entry + entry->length);
	}

	if (max_entries && count > max_entries) {
		pr_warn("[%4.4s:0x%02x] ignored %i entries of %i found\n",
			id, proc->id, count - max_entries, count);
	}

	return count;
}

int __init
acpi_parse_entries(char *id,
			unsigned long table_size,
			acpi_tbl_entry_handler handler,
			struct acpi_table_header *table_header,
			int entry_id, unsigned int max_entries)
{
	struct acpi_subtable_proc proc = {
		.id		= entry_id,
		.handler	= handler,
	};

	return acpi_parse_entries_array(id, table_size, table_header,
			&proc, 1, max_entries);
}

int __init
acpi_table_parse_entries_array(char *id,
			 unsigned long table_size,
			 struct acpi_subtable_proc *proc, int proc_num,
			 unsigned int max_entries)
{
	struct acpi_table_header *table_header = NULL;
	acpi_size tbl_size;
	int count;
	u32 instance = 0;

	if (acpi_disabled)
		return -ENODEV;

	if (!id)
		return -EINVAL;

	if (!strncmp(id, ACPI_SIG_MADT, 4))
		instance = acpi_apic_instance;

	acpi_get_table_with_size(id, instance, &table_header, &tbl_size);
	if (!table_header) {
		pr_warn("%4.4s not present\n", id);
		return -ENODEV;
	}

	count = acpi_parse_entries_array(id, table_size, table_header,
			proc, proc_num, max_entries);

	early_acpi_os_unmap_memory((char *)table_header, tbl_size);
	return count;
}

int __init
acpi_table_parse_entries(char *id,
			unsigned long table_size,
			int entry_id,
			acpi_tbl_entry_handler handler,
			unsigned int max_entries)
{
	struct acpi_subtable_proc proc = {
		.id		= entry_id,
		.handler	= handler,
	};

	return acpi_table_parse_entries_array(id, table_size, &proc, 1,
						max_entries);
}

int __init
acpi_table_parse_madt(enum acpi_madt_type id,
		      acpi_tbl_entry_handler handler, unsigned int max_entries)
{
	return acpi_table_parse_entries(ACPI_SIG_MADT,
					    sizeof(struct acpi_table_madt), id,
					    handler, max_entries);
}

/**
 * acpi_table_parse - find table with @id, run @handler on it
 * @id: table id to find
 * @handler: handler to run
 *
 * Scan the ACPI System Descriptor Table (STD) for a table matching @id,
 * run @handler on it.
 *
 * Return 0 if table found, -errno if not.
 */
int __init acpi_table_parse(char *id, acpi_tbl_table_handler handler)
{
	struct acpi_table_header *table = NULL;
	acpi_size tbl_size;

	if (acpi_disabled)
		return -ENODEV;

	if (!id || !handler)
		return -EINVAL;

	if (strncmp(id, ACPI_SIG_MADT, 4) == 0)
		acpi_get_table_with_size(id, acpi_apic_instance, &table, &tbl_size);
	else
		acpi_get_table_with_size(id, 0, &table, &tbl_size);

	if (table) {
		handler(table);
		early_acpi_os_unmap_memory(table, tbl_size);
		return 0;
	} else
		return -ENODEV;
}

/*
 * The BIOS is supposed to supply a single APIC/MADT,
 * but some report two.  Provide a knob to use either.
 * (don't you wish instance 0 and 1 were not the same?)
 */
static void __init check_multiple_madt(void)
{
	struct acpi_table_header *table = NULL;
	acpi_size tbl_size;

	acpi_get_table_with_size(ACPI_SIG_MADT, 2, &table, &tbl_size);
	if (table) {
		pr_warn("BIOS bug: multiple APIC/MADT found, using %d\n",
			acpi_apic_instance);
		pr_warn("If \"acpi_apic_instance=%d\" works better, "
			"notify linux-acpi@vger.kernel.org\n",
			acpi_apic_instance ? 0 : 2);
		early_acpi_os_unmap_memory(table, tbl_size);

	} else
		acpi_apic_instance = 0;

	return;
}

/*
 * acpi_table_init()
 *
 * find RSDP, find and checksum SDT/XSDT.
 * checksum all tables, print SDT/XSDT
 *
 * result: sdt_entry[] is initialized
 */

int __init acpi_table_init(void)
{
	acpi_status status;

	if (acpi_verify_table_checksum) {
		pr_info("Early table checksum verification enabled\n");
		acpi_gbl_verify_table_checksum = TRUE;
	} else {
		pr_info("Early table checksum verification disabled\n");
		acpi_gbl_verify_table_checksum = FALSE;
	}

	status = acpi_initialize_tables(initial_tables, ACPI_MAX_TABLES, 0);
	if (ACPI_FAILURE(status))
		return -EINVAL;

	check_multiple_madt();
	return 0;
}

static int __init acpi_parse_apic_instance(char *str)
{
	if (!str)
		return -EINVAL;

	if (kstrtoint(str, 0, &acpi_apic_instance))
		return -EINVAL;

	pr_notice("Shall use APIC/MADT table %d\n", acpi_apic_instance);

	return 0;
}

early_param("acpi_apic_instance", acpi_parse_apic_instance);

static int __init acpi_force_table_verification_setup(char *s)
{
	acpi_verify_table_checksum = true;

	return 0;
}

early_param("acpi_force_table_verification", acpi_force_table_verification_setup);
