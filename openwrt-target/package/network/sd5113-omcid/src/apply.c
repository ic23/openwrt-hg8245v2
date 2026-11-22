/* SPDX-License-Identifier: GPL-2.0-only */
/*
  * Phase 1a does not program the GPE. Every mutation leaves a fixed phrase.
  * No phrase carries attribute bytes: 148, 157 and the other opaque
  * stores cannot leak credentials into the log.
  */
#include "omci.h"

#include <stdio.h>

void omci_record_mutation(uint8_t apply, int deleting, int touched_admin,
			  int touched_alloc, struct omci_sink *sink)
{
	const char *base = NULL;
	char buf[160];

	if (!sink || !sink->record)
		return;
	switch (apply) {
	case OMCI_APPLY_NONE:
		return;
	case OMCI_APPLY_BRIDGE:
		base = "would apply ONU_GPE_BRIDGE_TABLE";
		break;
	case OMCI_APPLY_BRIDGE_PORT:
		base = "would apply FIO_GPE_BRIDGE_PORT_CFG_SET";
		break;
	case OMCI_APPLY_VLAN_FILTER:
		base = "would apply FIO_GPE_TAGGING_FILTER_SET";
		break;
	case OMCI_APPLY_PMAPPER:
		base = "would apply ONU_GPE_PMAPPER_TABLE";
		break;
	case OMCI_APPLY_EXT_VLAN:
		base = "would apply FIO_GPE_EXT_VLAN_SET";
		break;
	case OMCI_APPLY_GEM_IW:
		base = "would apply GEM IW (no ME ioctl; materialised by the mapper)";
		break;
	case OMCI_APPLY_GEM_PORT:
		base = "would apply FIO_GPE_GEM_PORT_ADD/SET/DELETE";
		break;
	case OMCI_APPLY_TCONT:
		if (touched_alloc)
			base = "would apply FIO_GPE_TCONT_SET";
		break;
	case OMCI_APPLY_QUEUE:
		base = "would apply FIO_GPE_EGRESS_QUEUE / SCHEDULER";
		break;
	case OMCI_APPLY_ONU_G:
		if (touched_admin)
			base = "ONU-G admin changed in the MIB; TX is left alone (PORT-TODO: GTC disable not identified)";
		break;
	case OMCI_APPLY_ANI:
		base = "stored; the laser is not changed";
		break;
	case OMCI_APPLY_LASER:
		base = "laser mode not changed";
		break;
	case OMCI_APPLY_STORE:
		base = "store only; GPE not applied";
		break;
	case OMCI_APPLY_DEBUG:
		base = "would apply debug (not executed)";
		break;
	default:
		return;
	}
	if (!base)
		return;
	if (deleting && apply != OMCI_APPLY_STORE) {
		snprintf(buf, sizeof buf, "%s (borrar)", base);
		sink->record(sink->ctx, buf);
	} else {
		sink->record(sink->ctx, base);
	}
}
