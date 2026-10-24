/* Dandelion boot shim - FDT parser implementation
 * Based on mouseos fdt.c, adapted for dandelion.
 */
#include "fdt.h"

static uint32_t be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const unsigned char *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
static int equal(const unsigned char *p, size_t n, const char *s)
{
	size_t i = 0;
	while (i < n && s[i] && p[i] == (unsigned char)s[i]) i++;
	return i == n && !s[i];
}
static uint64_t cells(const unsigned char *p, unsigned count)
{
	return count == 2 ? be64(p) : be32(p);
}
static int append(struct fdt_range *ranges, unsigned *count, unsigned max,
		  uint64_t base, uint64_t size)
{
	/* Ranges past the table limit or wrapping around are ignored. */
	if (!size || *count == max || base + size < base) return 0;
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;
	return 0;
}

struct frame {
	unsigned address_cells, size_cells;
	unsigned child_address_cells, child_size_cells;
	unsigned memory, reserved_root, reserved_child, chosen;
};

int fdt_inspect(const void *data, size_t available, struct fdt_info *info)
{
	const unsigned char *b = data;
	struct frame frames[16];
	unsigned depth = 0, root_seen = 0;

	if (available < 40 || be32(b) != 0xd00dfeed) return -1;

	uint32_t total = be32(b + 4), off = be32(b + 8);
	uint32_t strings = be32(b + 12), reserve = be32(b + 16);
	uint32_t ssize = be32(b + 32), tsize = be32(b + 36);

	if (total > available || total > 0x100000 || total < 40 ||
	    be32(b + 20) != 17 || off < 40 || (off & 3) || off > total ||
	    tsize > total - off || strings > total || ssize > total - strings ||
	    reserve < 40 || (reserve & 7))
		return -1;

	info->bytes = total;
	info->memory_count = info->reserved_count = 0;
	info->initrd_start = info->initrd_end = 0;

	/* Parse reserved memory map */
	for (;;) {
		if (reserve > total || total - reserve < 16) return -1;
		uint64_t base = be64(b + reserve), size = be64(b + reserve + 8);
		reserve += 16;
		if (!base && !size) break;
		if (append(info->reserved, &info->reserved_count, FDT_MAX_RESERVED, base, size))
			return -1;
	}

	/* Parse structure block */
	size_t end = off + tsize, p = off;
	while (p <= end && end - p >= 4) {
		unsigned token = be32(b + p);
		p += 4;

		if (token == 1) { /* BEGIN_NODE */
			if (depth == 16 || (!depth && root_seen)) return -1;
			size_t name = p;
			while (p < end && b[p]) p++;
			size_t length = p - name;
			p = (p + 4) & ~(size_t)3;

			struct frame *f = &frames[depth];
			f->address_cells = depth ? frames[depth-1].child_address_cells : 2;
			f->size_cells = depth ? frames[depth-1].child_size_cells : 1;
			f->child_address_cells = 2;
			f->child_size_cells = 1;

			size_t kind_length = 0;
			while (kind_length < length && b[name + kind_length] != '@')
				kind_length++;
			f->memory = depth == 1 && equal(b + name, kind_length, "memory");
			f->reserved_root = depth == 1 && equal(b + name, length, "reserved-memory");
			f->reserved_child = depth == 2 && frames[1].reserved_root;
			f->chosen = depth == 1 && equal(b + name, length, "chosen");

			if (!depth) { if (length) return -1; root_seen = 1; }
			depth++;
		} else if (token == 2) { /* END_NODE */
			if (!depth) return -1;
			depth--;
		} else if (token == 3) { /* PROP */
			if (!depth || end - p < 8) return -1;
			uint32_t length = be32(b + p), nameoff = be32(b + p + 4);
			p += 8;
			if (length > end - p || nameoff >= ssize) return -1;

			size_t n = 0;
			while (n < ssize - nameoff && b[strings + nameoff + n]) n++;
			if (n == ssize - nameoff) return -1;

			const unsigned char *key = b + strings + nameoff;
			struct frame *f = &frames[depth - 1];

			if (equal(key, n, "#address-cells")) {
				if (length != 4) return -1;
				uint32_t v = be32(b + p);
				/*
				 * Nodes with more than two address cells (PCIe
				 * host bridges) exist on some trees.  Nothing we
				 * care about lives below them, so clamp instead
				 * of rejecting the whole tree.
				 */
				f->child_address_cells = v <= 2 ? v : 2;
			} else if (equal(key, n, "#size-cells")) {
				if (length != 4) return -1;
				uint32_t v = be32(b + p);
				f->child_size_cells = v <= 2 ? v : 2;
			} else if ((f->memory || f->reserved_child) && equal(key, n, "reg")) {
				unsigned ac = f->address_cells, sc = f->size_cells;
				unsigned step = 4 * (ac + sc);
				if (!ac || !step || length % step) continue;
				for (size_t at = 0; at < length; at += step) {
					uint64_t base = cells(b + p + at, ac);
					uint64_t size = cells(b + p + at + 4 * ac, sc);
					if (f->memory)
						append(info->memory, &info->memory_count, FDT_MAX_MEMORY, base, size);
					else
						append(info->reserved, &info->reserved_count, FDT_MAX_RESERVED, base, size);
				}
			} else if (f->chosen && equal(key, n, "linux,initrd-start")) {
				if (length != 4 && length != 8) return -1;
				info->initrd_start = cells(b + p, length / 4);
			} else if (f->chosen && equal(key, n, "linux,initrd-end")) {
				if (length != 4 && length != 8) return -1;
				info->initrd_end = cells(b + p, length / 4);
			}
			p = (p + length + 3) & ~(size_t)3;
		} else if (token == 4) { /* NOP */
			continue;
		} else if (token == 9) { /* END */
			return root_seen && !depth && info->memory_count ? 0 : -1;
		} else {
			return -1;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Platform probe: find the hardware described by the device DTB      */
/* ------------------------------------------------------------------ */

#define PROBE_GIC   (1u << 0)
#define PROBE_TIMER (1u << 1)
#define PROBE_UART  (1u << 2)
#define PROBE_WDT   (1u << 3)
#define PROBE_CPU   (1u << 4)

#define PROBE_MAX_REG 3
#define PROBE_MAX_IRQ 8

/* Console UART on Dandelion (DANDELION_UART_BASE). */
#define PROBE_CONSOLE_UART 0x11002000ULL

struct probe_frame {
	unsigned address_cells, size_cells;
	unsigned child_address_cells, child_size_cells;

	unsigned flags;
	unsigned regs;
	uint64_t reg_base[PROBE_MAX_REG];
	uint64_t reg_size[PROBE_MAX_REG];
	unsigned irqs;
	uint32_t irq[PROBE_MAX_IRQ];
	uint32_t frequency;
	char compatible[FDT_COMPAT_LEN];
};

static uint64_t probe_cells(const unsigned char *p, unsigned count)
{
	if (count == 0) return 0;
	return count == 2 ? be64(p) : be32(p);
}

static void probe_copy(char *dst, size_t capacity,
		       const unsigned char *src, size_t length)
{
	if (length >= capacity) length = capacity - 1;
	for (size_t i = 0; i < length; i++) dst[i] = (char)src[i];
	dst[length] = 0;
}

static void probe_strcpy(char *dst, size_t capacity, const char *src)
{
	size_t i = 0;
	while (i + 1 < capacity && src[i]) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

static void probe_classify(struct probe_frame *f,
			   const unsigned char *s, size_t n)
{
	unsigned flag = 0;

	if (equal(s, n, "arm,gic-v3"))
		flag = PROBE_GIC;
	else if (equal(s, n, "arm,armv8-timer"))
		flag = PROBE_TIMER;
	else if (equal(s, n, "mediatek,mt6577-uart") ||
		 equal(s, n, "ns16550a") ||
		 equal(s, n, "snps,dw-apb-uart"))
		flag = PROBE_UART;
	else if (equal(s, n, "mediatek,mt6589-wdt") ||
		 equal(s, n, "mediatek,mt6765-wdt"))
		flag = PROBE_WDT;
	else if (equal(s, n, "arm,cortex-a53"))
		flag = PROBE_CPU;

	if (flag) {
		f->flags |= flag;
		if (!f->compatible[0])
			probe_copy(f->compatible, sizeof(f->compatible), s, n);
	}
}

static void probe_publish(struct fdt_platform *out, const struct probe_frame *f)
{
	if ((f->flags & PROBE_GIC) && f->regs) {
		unsigned n = f->regs < FDT_MAX_GIC_REGIONS ?
		    f->regs : FDT_MAX_GIC_REGIONS;
		out->gic.found = 1;
		out->gic.regions = n;
		for (unsigned i = 0; i < n; i++) {
			out->gic.base[i] = f->reg_base[i];
			out->gic.size[i] = f->reg_size[i];
		}
		probe_strcpy(out->gic.compatible, sizeof(out->gic.compatible),
			     f->compatible);
	}

	if (f->flags & PROBE_TIMER) {
		out->timer.found = 1;
		out->timer.frequency = f->frequency;
		out->timer.irq_count = f->irqs;
		for (unsigned i = 0; i < f->irqs && i < FDT_MAX_TIMER_IRQS; i++)
			out->timer.irq[i] = f->irq[i];
		probe_strcpy(out->timer.compatible, sizeof(out->timer.compatible),
			     f->compatible);
	}

	if ((f->flags & PROBE_UART) && f->regs) {
		/* Prefer the console UART when several are described. */
		if (!out->uart.found || f->reg_base[0] == PROBE_CONSOLE_UART) {
			out->uart.found = 1;
			out->uart.base = f->reg_base[0];
			out->uart.size = f->reg_size[0];
			probe_strcpy(out->uart.compatible,
				     sizeof(out->uart.compatible), f->compatible);
		}
	}

	if ((f->flags & PROBE_WDT) && f->regs) {
		if (!out->wdt.found) {
			out->wdt.found = 1;
			out->wdt.base = f->reg_base[0];
			out->wdt.size = f->reg_size[0];
			probe_strcpy(out->wdt.compatible,
				     sizeof(out->wdt.compatible), f->compatible);
		}
	}

	if (f->flags & PROBE_CPU) {
		if (out->cpus.count < FDT_MAX_CPUS)
			out->cpus.mpidr[out->cpus.count++] =
			    f->regs ? f->reg_base[0] : 0;
	}
}

int fdt_probe_platform(const void *data, size_t available,
		       struct fdt_platform *out)
{
	const unsigned char *b = data;
	struct probe_frame frames[16];
	unsigned depth = 0, root_seen = 0;

	if (!out) return -1;
	for (size_t i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;

	if (available < 40 || be32(b) != 0xd00dfeed) return -1;

	uint32_t total = be32(b + 4), off = be32(b + 8);
	uint32_t strings = be32(b + 12);
	uint32_t ssize = be32(b + 32), tsize = be32(b + 36);

	if (total > available || total > 0x100000 || total < 40 ||
	    be32(b + 20) != 17 || off < 40 || (off & 3) || off > total ||
	    tsize > total - off || strings > total || ssize > total - strings)
		return -1;

	size_t end = off + tsize, p = off;
	while (p <= end && end - p >= 4) {
		unsigned token = be32(b + p);
		p += 4;

		if (token == 1) { /* BEGIN_NODE */
			if (depth == 16 || (!depth && root_seen)) return -1;

			size_t name = p;
			while (p < end && b[p]) p++;
			if (p >= end) return -1;
			if (!depth) {
				if (p != name) return -1;  /* root name is empty */
				root_seen = 1;
			}
			p = (p + 4) & ~(size_t)3;

			struct probe_frame *f = &frames[depth];
			for (size_t i = 0; i < sizeof(*f); i++)
				((unsigned char *)f)[i] = 0;

			f->address_cells = depth ? frames[depth - 1].child_address_cells : 2;
			f->size_cells = depth ? frames[depth - 1].child_size_cells : 1;
			f->child_address_cells = 2;
			f->child_size_cells = 1;

			depth++;
		} else if (token == 2) { /* END_NODE */
			if (!depth) return -1;
			probe_publish(out, &frames[depth - 1]);
			depth--;
		} else if (token == 3) { /* PROP */
			if (!depth || end - p < 8) return -1;
			uint32_t length = be32(b + p), nameoff = be32(b + p + 4);
			p += 8;
			if (length > end - p || nameoff >= ssize) return -1;

			size_t n = 0;
			while (n < ssize - nameoff && b[strings + nameoff + n]) n++;
			if (n == ssize - nameoff) return -1;

			const unsigned char *key = b + strings + nameoff;
			const unsigned char *value = b + p;
			struct probe_frame *f = &frames[depth - 1];

			if (equal(key, n, "#address-cells")) {
				if (length == 4) {
					uint32_t v = be32(value);
					f->child_address_cells = v > 2 ? 2 : v;
				}
			} else if (equal(key, n, "#size-cells")) {
				if (length == 4) {
					uint32_t v = be32(value);
					f->child_size_cells = v > 2 ? 2 : v;
				}
			} else if (equal(key, n, "compatible")) {
				size_t at = 0;
				while (at < length) {
					const unsigned char *s = value + at;
					size_t len = 0;
					while (at + len < length && s[len]) len++;
					if (at + len >= length) return -1;
					probe_classify(f, s, len);
					at += len + 1;
				}
			} else if (equal(key, n, "reg")) {
				unsigned ac = f->address_cells, sc = f->size_cells;
				unsigned step = 4 * (ac + sc);
				/* Ignore odd reg layouts instead of rejecting the DTB. */
				if (ac && step && length % step == 0) {
					for (size_t at = 0;
					     at < length && f->regs < PROBE_MAX_REG;
					     at += step) {
						f->reg_base[f->regs] = probe_cells(value + at, ac);
						f->reg_size[f->regs] =
						    probe_cells(value + at + 4 * ac, sc);
						f->regs++;
					}
				}
			} else if (equal(key, n, "interrupts")) {
				if (length % 12 == 0) {
					for (size_t at = 0;
					     at + 12 <= length && f->irqs < PROBE_MAX_IRQ;
					     at += 12) {
						uint32_t type = be32(value + at);
						uint32_t num = be32(value + at + 4);
						uint32_t intid;
						if (type == 0)
							intid = num;          /* SPI */
						else if (type == 1)
							intid = 16 + num;     /* PPI */
						else
							intid = 1020 + num;   /* SGI */
						f->irq[f->irqs++] = intid;
					}
				}
			} else if (equal(key, n, "clock-frequency")) {
				if (length == 4)
					f->frequency = be32(value);
				else if (length == 8)
					f->frequency = (uint32_t)be64(value);
			} else if (equal(key, n, "enable-method")) {
				probe_copy(out->cpus.enable_method,
					   sizeof(out->cpus.enable_method),
					   value, length);
			}
			p = (p + length + 3) & ~(size_t)3;
		} else if (token == 4) { /* NOP */
			continue;
		} else if (token == 9) { /* END */
			return root_seen && !depth ? 0 : -1;
		} else {
			return -1;
		}
	}
	return -1;
}
