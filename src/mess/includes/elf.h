#ifndef __INCLUDES_ELF__
#define __INCLUDES_ELF__

#define SCREEN_TAG		"screen"
#define CDP1802_TAG		"a6"
#define CDP1861_TAG		"a14"
#define _74C923_TAG		"a10"
#define DM9368_L_TAG	"a12"
#define DM9368_H_TAG	"a8"
#define CASSETTE_TAG	"cassette"

typedef struct _elf_state elf_state;
struct _elf_state
{
	/* keyboard state */
	int keylatch;

	/* display state */
	int cdp1861_efx;				/* EFx */

	/* devices */
	const device_config *cdp1861;
	const device_config *_74c923;
	const device_config *dm9368_l;
	const device_config *dm9368_h;
	const device_config *cassette;
};

#endif
