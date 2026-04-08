#include <zephyr/linker/iterable_sections.h>
// todo: use number of net_if devices as number of allocated ets instances
ITERABLE_SECTION_RAM(ats, 1)