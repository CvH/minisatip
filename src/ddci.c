/*
 * Copyright (C) 2016 Catalin Toda <catalinii@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#include <ctype.h>

#include <linux/dvb/ca.h>
#include "dvb.h"
#include "socketworks.h"
#include "minisatip.h"
#include "ddci.h"
#include "utils.h"
#include "tables.h"

#define DEFAULT_LOG LOG_DVBCA

int ddci_adapters;
extern int dvbca_id;
extern SCA ca[MAX_CA];

int first_ddci = -1;

#define get_ddci(i) ((i >= 0 && i < MAX_ADAPTERS && ddci_devices[i] && ddci_devices[i]->enabled) ? ddci_devices[i] : NULL)

int ddci_id;
ddci_device_t *ddci_devices[MAX_ADAPTERS];

int mapping_table_pids;
typedef struct ddci_mapping_table
{
	int ad_pid;
	int ddci_adapter;
	int ddci_pid;
	char rewrite;
	int pmt[MAX_CHANNELS_ON_CI + 1];
	int npmt;
	int filter_id;
} ddci_mapping_table_t;

ddci_mapping_table_t *mapping_table;

int process_cat(int filter, unsigned char *b, int len, void *opaque);

int add_pid_ddci(int ddci_adapter, int pid, int ddci_pid, int idx)
{
	ddci_device_t *d = get_ddci(ddci_adapter);
	if (!d)
		LOG_AND_RETURN(-1, "ddci_adapter %d disabled", ddci_adapter);
	if (pid < 0 || pid > 8191)
		LOG_AND_RETURN(-1, "pid %d invalid", pid);

	if (ddci_pid >= 0 && d->pid_mapping[ddci_pid] == idx)
		return ddci_pid;

	for (ddci_pid = pid; (ddci_pid & 0xFFFF) < 8191; ddci_pid++)
	{
		if (d->pid_mapping[ddci_pid] == -1)
		{
			d->pid_mapping[ddci_pid] = idx;
			return ddci_pid;
		}
	}
	return -1;
}
int add_pid_mapping_table(int ad, int pid, int pmt, int ddci_adapter)
{
	int ddci_pid = 0, i;
	int key = (ad << 16) | pid;
	if (!mapping_table)
		return -1;
	int idx = get_index_hash(&mapping_table[0].ad_pid, mapping_table_pids, sizeof(ddci_mapping_table_t), key, key);
	if (idx == -1)
		idx = get_index_hash(&mapping_table[0].ad_pid, mapping_table_pids, sizeof(ddci_mapping_table_t), key, (uint32_t)-1);
	if (idx == -1)
		return -1;

	ddci_pid = add_pid_ddci(ddci_adapter, pid, mapping_table[idx].ddci_pid, idx);
	if (ddci_pid == -1)
		LOG_AND_RETURN(-1, "could not add pid %d and ad %d to the mapping table", pid, ad);
	mapping_table[idx].ad_pid = key;
	mapping_table[idx].ddci_adapter = ddci_adapter;
	mapping_table[idx].ddci_pid = ddci_pid;
	mapping_table[idx].rewrite = 1;

	int add_pid = 1, add_pmt = 1;
	for (i = 0; i < mapping_table[idx].npmt; i++)
	{
		if (mapping_table[idx].pmt[i] >= 0)
			add_pid = 0;
		if (mapping_table[idx].pmt[i] == pmt)
			add_pmt = 0;
	}
	if (add_pmt)
		for (i = 0; i < MAX_CHANNELS_ON_CI; i++)
		{
			if (mapping_table[idx].pmt[i] < 0)
			{
				mapping_table[idx].pmt[i] = pmt;
				if (i >= mapping_table[idx].npmt)
					mapping_table[idx].npmt = i + 1;
				break;
			}
		}
	if (add_pid)
	{
		if (pid != 1)
			mark_pid_add(-1, ad, pid);
		else
			mapping_table[idx].filter_id = add_filter(ad, 1, (void *)process_cat, get_ddci(ddci_adapter), FILTER_CRC);
		mark_pid_add(-1, ddci_adapter, ddci_pid);
	}
	LOG("mapped adapter %d pid %d to %d", ad, pid, ddci_pid);
	return ddci_pid;
}

inline static int get_mapping_table(int ad, int pid, int *ddci_adapter, int *rewrite)
{
	int key = (ad << 16) | pid;
	int idx = get_index_hash(&mapping_table[0].ad_pid, mapping_table_pids, sizeof(ddci_mapping_table_t), key, key);
	if (idx == -1)
		return -1;
	if (ddci_adapter)
		*ddci_adapter = mapping_table[idx].ddci_adapter;
	if (rewrite)
		*rewrite = mapping_table[idx].rewrite;
	return mapping_table[idx].ddci_pid;
}

int set_pid_rewrite(int ad, int pid, int rewrite)
{
	int key = (ad << 16) | pid;
	int idx = get_index_hash(&mapping_table[0].ad_pid, mapping_table_pids, sizeof(ddci_mapping_table_t), key, key);
	if (idx == -1)
		return -1;
	mapping_table[idx].rewrite = rewrite;
	return 0;
}

int del_pid_ddci(int ddci_adapter, int ddci_pid, int pmt)
{
	ddci_device_t *d = get_ddci(ddci_adapter);
	if (!d)
		LOG_AND_RETURN(-1, "ddci_adapter %d disabled", ddci_adapter);
	d->pid_mapping[ddci_pid] = -1;

	return 0;
}

int del_pid_mapping_table(int ad, int pid, int pmt)
{
	int ddci_pid, ddci_adapter, i;
	int filter_id;
	int key = (ad << 16) | pid;
	int idx = get_index_hash(&mapping_table[0].ad_pid, mapping_table_pids, sizeof(ddci_mapping_table_t), key, key);
	if (idx == -1)
		return -1;
	ddci_pid = mapping_table[idx].ddci_pid;
	ddci_adapter = mapping_table[idx].ddci_adapter;
	filter_id = mapping_table[idx].filter_id;
	mapping_table[idx].ad_pid = -1;
	mapping_table[idx].ddci_adapter = -1;
	mapping_table[idx].ddci_pid = -1;
	mapping_table[idx].rewrite = 0;
	mapping_table[idx].filter_id = -1;

	int del_pid = 1;
	for (i = 0; i < mapping_table[idx].npmt; i++)
	{
		if (mapping_table[idx].pmt[i] == pmt)
			mapping_table[idx].pmt[i] = -1;
		if (mapping_table[idx].pmt[i] >= 0)
			del_pid = 0;
	}
	if (del_pid)
	{
		SPid *p = find_pid(ad, pid);
		LOGM("No pmt found for ad %d pid %d, deleteing if not used %d", ad, pid, p ? p->sid[0] : -2);
		if (pid == 1)
			del_filter(filter_id);
		else if (p && p->sid[0] == -1)
		{
			mark_pid_deleted(ad, -1, pid, NULL);
		}
	}
	return del_pid_ddci(ddci_adapter, ddci_pid, pmt);
}

int ddci_init_dev(adapter *ad)
{
	return TABLES_RESULT_OK;
}

int ddci_close_dev(adapter *ad)
{
	if (ad->fe_sock > 0)
		sockets_del(ad->fe_sock);
	ad->fe_sock = -1;
	return TABLES_RESULT_OK;
}
SCA_op ddci;

void ddci_close_device(ddci_device_t *c)
{
}

int ddci_close_all()
{
	int i;
	for (i = 0; i < MAX_ADAPTERS; i++)
		if (ddci_devices[i] && ddci_devices[i]->enabled)
		{
			ddci_close_device(ddci_devices[i]);
		}
	return 0;
}

int ddci_close(adapter *a)
{
	return 0;
}

int find_ddci_for_pmt(SPMT *pmt)
{
	int i;
	ddci_device_t *d;
	for (i = 0; i < MAX_ADAPTERS; i++)
		if ((d = get_ddci(i)))
		{
			int j;
			for (j = 0; j < ca[dvbca_id].ad_info[i].caids; j++)
				if (match_caid(pmt, ca[dvbca_id].ad_info[i].caid[j], ca[dvbca_id].ad_info[i].mask[j]))
				{
					LOG("DDCI %d CAID %04X and mask %04X matched PMT %d", i, ca[dvbca_id].ad_info[i].caid[j], ca[dvbca_id].ad_info[i].mask[j], pmt->id);
					if (d->channels < d->max_channels)
						return d->id;
				}
		}
	return -1;
}

// determine if the pids from this PMT needs to be added to the virtual adapter, also adds the PIDs to the translation table
int ddci_process_pmt(adapter *ad, SPMT *pmt)
{
	int i, ddid = 0;
	int add_pmt = 0;
	int rv = TABLES_RESULT_ERROR_NORETRY;

	LOGM("%s: start adapter %d and pmt %d", __FUNCTION__, ad->id, pmt->id);
	ddid = find_ddci_for_pmt(pmt);
#ifdef DDCI_TEST
	ddid = first_ddci;
#endif
	ddci_device_t *d = get_ddci(ddid);
	if (!d)
	{
		LOGM("Could not find ddci device for adapter %d", ad->id);
		return TABLES_RESULT_ERROR_NORETRY;
	}
	mutex_lock(&d->mutex);

	LOGM("found device %d for pmt %d", ddid, pmt->id);
	for (i = 0; i < d->max_channels; i++)
		if (d->pmt[i] == -1)
		{
			d->pmt[i] = pmt->id;
			add_pmt = 1;
			break;
		}
	if (!add_pmt)
	{
		LOG("No free slot found for pmt %d on DDCI %d", pmt->id, d->id);
		mutex_unlock(&d->mutex);

		return TABLES_RESULT_ERROR_RETRY;
	}

	d->channels++;
	if (d->pmt[0] == pmt->id) //process the CAT only for the first PMT
	{
		add_pid_mapping_table(ad->id, 1, pmt->id, d->id); // add pid 1
	}

	add_pid_mapping_table(ad->id, pmt->pid, pmt->id, d->id);
	set_pid_rewrite(ad->id, pmt->pid, 0); // do not send the PMT pid to the DDCI device

	for (i = 0; i < pmt->caids; i++)
	{
		LOG("%s: Adding ECM pid %d", __FUNCTION__, pmt->capid[i]);
		add_pid_mapping_table(ad->id, pmt->capid[i], pmt->id, d->id);
	}

	for (i = 0; i < pmt->all_pids; i++)
	{
		LOG("%s: Adding ALL pid %d", __FUNCTION__, pmt->all_pid[i]);
		add_pid_mapping_table(ad->id, pmt->all_pid[i], pmt->id, d->id);
	}

	update_pids(ad->id);
	rv = TABLES_RESULT_OK;

	mutex_unlock(&d->mutex);
	return rv;
}

// if the PMT is used by the adapter, the pids will be removed from the translation table
int ddci_del_pmt(adapter *ad, SPMT *spmt)
{
	int ddid = 0, pid, i, pmt = spmt->id;
	pid = get_mapping_table(ad->id, spmt->pid, &ddid, NULL);
	ddci_device_t *d = get_ddci(ddid);
	if (!d)
		LOG_AND_RETURN(0, "%s: ddci %d already disabled", __FUNCTION__, ddid);
	LOG("%s: deleting pmt id %d, pid %d, ddci %d", __FUNCTION__, spmt->id, pid, ddid);
	if (d->pmt[0] == pmt)
		d->cat_processed = 0;

	for (i = 0; i < d->max_channels; i++)
		if (d->pmt[i] == pmt)
		{
			d->pmt[i] = -1;
		}
	for (i = 0; i < mapping_table_pids; i++)
		if ((mapping_table[i].ad_pid >= 0) && (mapping_table[i].ad_pid >> 16) == ad->id)
		{
			int j, need_delete = 0;
			for (j = 0; j < mapping_table[i].npmt; j++)
				if (mapping_table[i].pmt[j] == pmt)
				{
					need_delete = 1;
					break;
				}
			if (need_delete)
			{
				LOG("Deleting pid %d", mapping_table[i].ad_pid & 0xFFFF);
				del_pid_mapping_table(ad->id, mapping_table[i].ad_pid & 0xFFFF, pmt);
			}
			LOGM("pid %d does not have pmt %d", mapping_table[i].ad_pid & 0xFFFF, pmt);
		}
	update_pids(ad->id);
	return 0;
}

void set_pid_ts(unsigned char *b, int pid)
{
	pid &= 0x1FFF;
	b[1] &= 0xE0;
	b[1] |= (pid >> 8) & 0x1F;
	b[2] = pid & 0xFF;
}

int ddci_create_pat(ddci_device_t *d, uint8_t *b)
{
	int len = 0;
	int i, ddci;
	SPMT *pmt;
	b[0] = 0;
	b[1] = 0;
	b[2] = 0xb0;
	b[3] = 0; // len
	copy16(b, 4, d->tid);
	b[6] = 0xd | (d->ver << 1);
	b[7] = b[8] = 0;
	// Channel ID 0
	b[9] = b[10] = 0;
	// PID 16
	b[11] = 0x00;
	b[12] = 0x10;
	len = 13;
	for (i = 0; i < MAX_CHANNELS_ON_CI; i++)
		if ((pmt = get_pmt(d->pmt[i])))
		{
			int dpid = get_mapping_table(pmt->adapter, pmt->pid, &ddci, NULL);
			if (dpid == -1)
			{
				LOG("adapter %d pid %d not found in the mapping table", pmt->adapter, pmt->pid);
				continue;
			}

			if (ddci != d->id)
			{
				LOG("adapter %d pid %d not mapped to the right DDCI adapter %d != expected %d", pmt->adapter, pmt->pid, ddci, d->id);
				continue;
			}
			copy16(b, len, pmt->sid);
			copy16(b, len + 2, 0xE000 | dpid);
			len += 4;
		}
	int len1 = len;
	len += 4;
	b[2] |= (len1 >> 8);
	b[3] |= (len1 & 0xFF);
	uint32_t crc = crc_32(b + 1, len1 - 1);
	copy32(b, len1, crc);
	char buf[100];
	sprintf(buf, "PAT Created CRC %08X: ", crc);
	hexdump(buf, b + 1, len1 - 1);
	return len;
}

void ddci_replace_pi(int adapter, unsigned char *es, int len)
{

	int es_len, capid;
	int i;
	int dpid, ddci;

	for (i = 0; i < len; i += es_len) // reading program info
	{
		es_len = es[i + 1] + 2;
		if (es[i] != 9)
			continue;
		capid = (es[i + 4] & 0x1F) * 256 + es[i + 5];
		dpid = get_mapping_table(adapter, capid, &ddci, NULL);
		if (dpid < 0)
			dpid = capid;

		es[i + 4] &= 0xE0; //~0x1F
		es[i + 4] |= (dpid >> 8);
		es[i + 5] = dpid & 0xFF;
		LOGM("%s: CA pid %d -> pid %d", __FUNCTION__, capid, dpid)
	}
	return;
}

int ddci_create_pmt(ddci_device_t *d, SPMT *pmt, uint8_t *clean)
{
	int len = pmt->pmt_len;
	int ddci, pid = pmt->pid, pi_len, pmt_len;
	int es_len, i, spid, dpid;
	uint8_t *b, *pi, *pmt_b;
	clean[0] = 0;
	b = clean + 1;
	memcpy(b, pmt->pmt, len);
	pi_len = ((b[10] & 0xF) << 8) + b[11];
	pmt_len = pmt->pmt_len - 4;

	pi = b + 12;
	pmt_b = pi + pi_len;

	if (pi_len > pmt_len)
		pi_len = 0;

	if (pi_len > 0)
		ddci_replace_pi(pmt->adapter, pi, pi_len);

	LOGM("%s: PMT %d AD %d, pid: %04X (%d), pmt_len %d, pi_len %d, total_len %d, sid %04X (%d) %s %s",
		 __FUNCTION__, pmt->id, pmt->adapter, pid, pid, pmt_len, pi_len, pmt_len - pi_len - 13, pmt->sid, pmt->sid, pmt->name[0] ? "channel:" : "", pmt->name);

	es_len = 0;
	pmt->active_pids = 0;
	pmt->active = 1;
	for (i = 0; i < pmt_len - pi_len - 13; i += (es_len) + 5) // reading streams
	{
		es_len = (pmt_b[i + 3] & 0xF) * 256 + pmt_b[i + 4];
		spid = (pmt_b[i + 1] & 0x1F) * 256 + pmt_b[i + 2];
		dpid = get_mapping_table(pmt->adapter, spid, &ddci, NULL);
		if (dpid < 0)
			dpid = spid;

		pmt_b[i + 1] &= 0xE0; //~0x1F
		pmt_b[i + 1] |= (dpid >> 8);
		pmt_b[i + 2] = dpid & 0xFF;

		LOGM("DDCI: PMT pid %d - stream pid %04X -> %04X es_len %d, pos %d",
			 pid, spid, dpid, es_len, i);
		if ((es_len + i + 5 > pmt_len) || (es_len < 0))
		{
			LOGM("pmt processing complete, es_len + i %d, len %d, es_len %d", es_len + i, pmt_len, es_len);
			break;
		}

		ddci_replace_pi(pmt->adapter, pmt_b + i + 5, es_len);
	}

	uint32_t crc = crc_32(b, pmt_len);
	copy32(b, pmt_len, crc);
	return pmt_len + 4 + 1;
}

int ddci_add_psi(ddci_device_t *d, uint8_t *dst, int len)
{
	unsigned char psi[1500];
	uint64_t ctime = getTick();
	int i, pos = 0;
	int psi_len;
	if (ctime - d->last_pat > 500)
	{
		psi_len = ddci_create_pat(d, psi);
		pos += buffer_to_ts(dst + pos, len - pos, psi, psi_len, &d->pat_cc, 0);
		d->last_pat = ctime;
	}

	if (ctime - d->last_pmt > 100)
	{
		SPMT *pmt;
		for (i = 0; i < MAX_CHANNELS_ON_CI; i++)
			if ((pmt = get_pmt(d->pmt[i])))
			{
				psi_len = ddci_create_pmt(d, pmt, psi);
				int dpid = get_mapping_table(pmt->adapter, pmt->pid, NULL, NULL);
				if (dpid != -1)
					pos += buffer_to_ts(dst + pos, len - pos, psi, psi_len, &d->pmt_cc[i], dpid);
				else
					LOG("%s: could not find PMT adapter %d and pid %d to mapping table", __FUNCTION__, pmt->adapter, pmt->pid);
			}
		d->last_pmt = ctime;
	}
	return pos;
}

int push_ts_to_adapter(adapter *ad, unsigned char *b, int new_pid, int *ad_pos)
{
	int i, new_pos = -1;
	for (i = *ad_pos; i < ad->rlen; i += 188)
		if (PID_FROM_TS(ad->buf + i) == 0x1FFF)
		{
			new_pos = i;
			break;
		}
	if ((new_pos == -1) && (ad->rlen <= ad->lbuf - 188))
	{
		new_pos = ad->rlen;
		ad->rlen += 188;
	}
	if (new_pos < 0 || new_pos + 188 > ad->lbuf)
	{
		LOGM("Could not push more data for adapter %d, rlen %d, lbuf %d, new pos %d", ad->id, ad->rlen, ad->lbuf, new_pos);
		*ad_pos = 0;
		return 1;
	}

	memcpy(ad->buf + new_pos, b, 188);
	set_pid_ts(ad->buf + new_pos, new_pid);
	set_pid_ts(b, 0x1FFF);
	LOGM("new position found is %d", new_pos)
	*ad_pos = new_pos;
	return 0;
}

// the packet is not needed -> 0, adapter buffer is full, stop processing -> 1

int copy_ts_from_ddci_buffer(adapter *ad, ddci_device_t *d, unsigned char *b, int *ad_pos)
{
	int pid = PID_FROM_TS(b);
	if (pid == 0x1FFF)
		return 0;

	int idx = d->pid_mapping[pid];

	if (idx == -1)
	{
		LOGM("DD %d pid %d not found in mapping table", d->id, pid);
		return 0;
	}

	int aid = mapping_table[idx].ad_pid >> 16;
	if (aid != ad->id)
	{
		//		LOG("ad %d, expected %d", aid, ad->id);
		return -1;
	}
	// search for a position to put the packet in
	int dpid = mapping_table[idx].ad_pid & 0x1FFF;
	DEBUGM("%s: mapping pid %d -> %d", __FUNCTION__, pid, dpid);
	if (push_ts_to_adapter(ad, b, dpid, ad_pos))
	{
		return 1;
	}
	return 0;
}

int ddci_process_ts(adapter *ad, ddci_device_t *d)
{
	unsigned char *b;
	adapter *ad2 = get_adapter(d->id);
	int rlen = ad->rlen;
	int bytes = 0;
	int iop = 0, iomax = ad->rlen / 188;
	int pid, dpid, i, ddci, rewrite;
	struct iovec io[iomax];
	if (mutex_lock(&d->mutex))
		return 0;
	if (!d->enabled)
		return 0;

	if (!ad2)
	{
		mutex_unlock(&d->mutex);
		return 0;
	}
	// step 1 - fill the IO with TS packets and change the PID as required by mapping table
	for (i = 0; i < rlen; i += 188)
	{
		b = ad->buf + i;
		if(b[0] != 0x47)
			continue;
		pid = PID_FROM_TS(b);
		dpid = get_mapping_table(ad->id, pid, &ddci, &rewrite);
		if (dpid == -1)
			continue;
		if (!rewrite)
			continue;
		if (ddci != d->id)
			continue;

		if (dpid != (dpid & 0x1FFF))
			LOG("%s: mapped pid not valid %d, source pid %d adapter %d", __FUNCTION__, dpid, pid, ad->id);

		set_pid_ts(b, dpid);
		io[iop].iov_base = b;
		io[iop].iov_len = 188;
		bytes += io[iop].iov_len;
		DEBUGM("pos %d of %d, mapping pid %d to %d", iop, rlen / 188, pid, dpid);
		iop++;
	}
	// write the TS to the DDCI handle
	if (iop > 0)
	{
		unsigned char psi[MAX_CHANNELS_ON_CI * 1500];
		int psi_len = ddci_add_psi(d, psi, sizeof(psi) - 1);
		hexdump("PSI -> ", psi, psi_len);
		if (psi_len > 0)
		{
			io[iop].iov_base = psi;
			io[iop].iov_len = psi_len;
			bytes += io[iop].iov_len; 
			iop++;
		}

		LOGM("writing %d bytes to DDCI device fd %d, sock %d", iop * 188, ad2->fe, ad2->fe_sock);
		int rb = writev(ad2->fe, io, iop);
		if (rb != bytes)
			LOG("%s: write incomplete to DDCI %d,fd %d, wrote %d out of %d, errno %d: %s", __FUNCTION__, ad2->id, ad2->fe, rb, bytes, errno, strerror(errno));
	}
	// mark the written TS packets as 8191 (0x1FFF)
	for (i = 0; i < iop; i++)
		set_pid_ts(io[i].iov_base, 0x1FFF);

	// move back TS packets from the DDCI out buffer to the adapter buffer
	int ad_pos = 0;
	LOGM("ddci_process_ts ro %d, wo %d max %d", d->ro, d->wo, DDCI_BUFFER);

	if ((d->ro % 188) != (d->wo % 188))
	{
		LOG("ddci %d, ro and wo not correctly alligned ro %d wo %d", d->ro, d->wo);
		mutex_unlock(&d->mutex);
		return 0;
	}

	for (i = d->ro; i != d->wo; i = (i + 188) % DDCI_BUFFER)
	{
		dump_packets("ddci_process_ts ->", d->out + i, 188, i);
		int rv = copy_ts_from_ddci_buffer(ad, d, d->out + i, &ad_pos);
		if (!rv && (d->ro == i))
			d->ro = (i + 188) % DDCI_BUFFER;
		if (rv == 1)
		{
			LOGM("adapter %d buffer full %d, dd %d, ro %d, wo %d", ad->id, d->id, d->ro, d->wo);
			break;
		}
	}

	mutex_unlock(&d->mutex);
	return 0;
}

int ddci_ts(adapter *ad)
{
	int i;

	// do not process the TS for DDCI devices
	if (ddci_devices[ad->id] && ddci_devices[ad->id]->enabled)
		return 0;

	for (i = 0; i < MAX_ADAPTERS; i++)
		if (ddci_devices[i] && ddci_devices[i]->enabled)
			ddci_process_ts(ad, ddci_devices[i]);

	return 0;
}

void ddci_init() // you can search the devices here and fill the ddci_devices, then open them here (for example independent CA devices), or use ddci_init_dev to open them (like in this module)
{
	memset(&ddci, 0, sizeof(ddci));
	ddci.ca_init_dev = ddci_init_dev;
	ddci.ca_close_dev = ddci_close_dev;
	ddci.ca_add_pmt = ddci_process_pmt;
	ddci.ca_del_pmt = ddci_del_pmt;
	ddci.ca_close_ca = ddci_close;
	ddci.ca_ts = ddci_ts;
	ddci_id = add_ca(&ddci, 0xFFFFFFFF);
}
int ddci_set_pid(adapter *a, int i_pid)
{
	return 100;
}

int ddci_del_filters(adapter *ad, int fd, int pid)
{
	return 0;
}

int push_ts_to_ddci_buffer(ddci_device_t *d, unsigned char *b, int rlen)
{
	int left, i, init_rlen = rlen;
	unsigned char *init_b = b;
	if (d->ro <= d->wo)
	{
		left = DDCI_BUFFER - d->wo;
		if (left > rlen)
			left = rlen;
		memcpy(d->out + d->wo, b, left);
		//		dump_packets("ddci_read_sec_data1 -> ", b, left, 0);
		rlen -= left;
		b += left;
		d->wo = (d->wo + left) % DDCI_BUFFER;
	}
	// do not overwrite the data where d->ro points to
	if (rlen > 0 && (d->ro - d->wo > 188))
	{
		left = d->ro - d->wo - 188;
		if (left > rlen)
			left = rlen;
		memcpy(d->out + d->wo, b, left);
		//		dump_packets("ddci_read_sec_data2 -> ", b, left, 0);
		rlen -= left;
		b += left;
		d->wo = (d->wo + left) % DDCI_BUFFER;
	}
	for (i = 0; i < init_rlen - rlen; i += 188)
	{
		init_b[i + 1] |= 0x1F;
		init_b[i + 2] |= 0xFF;
	}
	LOGM("%s: %d bytes, left %d, ro %d, wo %d", __FUNCTION__, init_rlen, rlen, d->ro, d->wo);
	return rlen;
}

int ddci_read_sec_data(sockets *s)
{
	unsigned char *b = s->buf;

	read_dmx(s);
	if (s->rlen != 0)
	{
		LOGM("process_dmx not called as s->rlen %d", s->rlen);
		return 0;
	}
	// copy the processed data to d->out buffer
	adapter *ad = get_adapter(s->sid);
	ddci_device_t *d = get_ddci(s->sid);
	b = ad->buf;
	int left = 0, rlen = ad->rlen;

	if ((left = push_ts_to_ddci_buffer(d, b, rlen)) > 0)
		LOG("dropping %d bytes for ddci_adapter %d", left, d->id);
	return 0;
}

void ddci_post_init(adapter *ad)
{
	sockets *s = get_sockets(ad->sock);
	s->action = (socket_action)ddci_read_sec_data;
	set_socket_thread(ad->fe_sock, get_socket_thread(ad->sock));
}

int ddci_open_device(adapter *ad)
{
	char buf[100];
	int read_fd, write_fd;
	ddci_device_t *d = ddci_devices[ad->id];
	if (!d)
	{
		unsigned char *out;
		out = malloc1(DDCI_BUFFER + 10);
		if (!out)
		{
			LOG_AND_RETURN(1, "%s: could not allocated memory for the output buffer for adapter %d", __FUNCTION__, ad->id);
		}

		d = ddci_devices[ad->id] = malloc1(sizeof(ddci_device_t));
		if (!d)
			return -1;
		mutex_init(&d->mutex);
		d->id = ad->id;
		memset(out, -1, DDCI_BUFFER + 10);
		d->out = out;
	}
	LOG("DDCI opening [%d] adapter %d and frontend %d", ad->id, ad->pa, ad->fn);
	sprintf(buf, "/dev/dvb/adapter%d/sec%d", ad->pa, ad->fn);
#ifndef DDCI_TEST
	write_fd = open(buf, O_WRONLY);
	if (write_fd < 0)
	{
		LOG("%s: could not open %s in WRONLY mode error %d: %s", __FUNCTION__, buf, errno, strerror(errno));
		return 1;
	}

	read_fd = open(buf, O_RDONLY);
	if (read_fd < 0)
	{
		LOG("%s: could not open %s in RDONLY mode error %d: %s", __FUNCTION__, buf, errno, strerror(errno));
		if (write_fd >= 0)
			close(write_fd);
		ad->fe = -1;
		return 1;
	}
#else
	int fd[2];
	if (pipe(fd) == -1)
	{
		LOG("pipe failed errno %d: %s", errno, strerror(errno));
		return 1;
	}
	read_fd = fd[0];
	write_fd = fd[1];

#endif
	mutex_lock(&d->mutex);
	ad->fe = write_fd;
	// create a sockets for buffering
	ad->fe_sock = sockets_add(ad->fe, NULL, ad->id, TYPE_TCP, NULL, NULL, NULL); 
	if(ad->fe_sock < 0)
		LOG_AND_RETURN(ad->fe_sock, "Failed to add sockets for the DDCI device");
	ad->dvr = read_fd;
	ad->type = ADAPTER_DVB;
	ad->dmx = -1;
	ad->sys[0] = 0;
	ad->adapter_timeout = 0;
	memset(d->pid_mapping, -1, sizeof(d->pid_mapping));
	memset(d->pmt, -1, sizeof(d->pmt));
	d->ncapid = 0;
	d->max_channels = MAX_CHANNELS_ON_CI;
	d->channels = 0;
	d->ro = d->wo = 0;
	d->last_pmt = d->last_pat = 0;
	d->tid = d->ver = 0;
	d->enabled = 1;
	ad->enabled = 1;
	mutex_unlock(&d->mutex);
	LOG("opened DDCI adapter %d fe:%d dvr:%d", ad->id, ad->fe, ad->dvr);

	return 0;
}

fe_delivery_system_t ddci_delsys(int aid, int fd, fe_delivery_system_t *sys)
{
	return 0;
}

int process_cat(int filter, unsigned char *b, int len, void *opaque)
{
	int cat_len = 0, i, es_len = 0, caid, add_cat = 1;
	ddci_device_t *d = (ddci_device_t *)opaque;
	cat_len = len - 4; // remove crc
	SFilter *f = get_filter(filter);
	int id;
	if (!f)
		return 0;

	if (b[0] != 1)
		return 0;

	if (!d->enabled)
		LOG_AND_RETURN(0, "DDCI %d no longer enabled, not processing PAT", d->id);

	if (d->cat_processed)
		return 0;

	cat_len -= 9;
	b += 8;
	LOG("CAT DDCI %d len %d", d->id, cat_len);
	if (cat_len > 1500)
		return 0;

	id = -1;
	for (i = 0; i < cat_len; i += es_len) // reading program info
	{
		es_len = b[i + 1] + 2;
		if (b[i] != 9)
			continue;
		caid = b[i + 2] * 256 + b[i + 3];
		if (++id < MAX_CA_PIDS)
			d->capid[id] = (b[i + 4] & 0x1F) * 256 + b[i + 5];

		LOG("CAT pos %d caid %d, pid %d", id, caid, d->capid[id]);
	}
	id++;

	add_cat = 1;
	mutex_lock(&d->mutex);
	for (i = 0; i < id; i++)
		if (d->pid_mapping[d->capid[i]] >= 0)
		{
			add_cat = 0;
			LOG("CAT pid %d already in use by index %d", d->capid[i], d->pid_mapping[d->capid[i]]);
			break;
		}
	if (!add_cat)
	{
		mutex_unlock(&d->mutex);
		return 0;
	}

	// sending EMM pids to the CAM
	for (i = 0; i < id; i++)
	{
		add_pid_mapping_table(f->adapter, d->capid[i], d->pmt[0], d->id);
	}
	d->cat_processed = 1;
	d->ncapid = id;
	mutex_unlock(&d->mutex);
	update_pids(f->adapter);
	return 0;
}

void find_ddci_adapter(adapter **a)
{
	int na = -1;
	char buf[100];
	int cnt;
	int i = 0, j = 0;

	ddci_adapters = 0;
	adapter *ad;
	if (opts.disable_dvb)
	{
		LOG("DVBCI device detection disabled");
		return;
	}

	for (i = 0; i < MAX_ADAPTERS; i++)
		if (a[i])
			na = i;
	na++;
	LOGM("Starting %s with index %d", __FUNCTION__, na);

	for (i = 0; i < MAX_ADAPTERS; i++)
		for (j = 0; j < MAX_ADAPTERS; j++)
		{
			cnt = 0;
			sprintf(buf, "/dev/dvb/adapter%d/ca%d", i, j);
			if (!access(buf, R_OK))
				cnt++;

			sprintf(buf, "/dev/dvb/adapter%d/sec%d", i, j);
			if (!access(buf, R_OK))
				cnt++;
#ifdef DDCI_TEST
			cnt = 2;
#endif
			if (cnt == 2)
			{
				LOGM("%s: adding %d %d to the list of devices", __FUNCTION__, i, j);
				if (!a[na])
					a[na] = adapter_alloc();

				ad = a[na];
				ad->pa = i;
				ad->fn = j;

				ad->open = (Open_device)ddci_open_device;
				ad->commit = (Adapter_commit)NULL;
				ad->tune = (Tune)NULL;
				ad->delsys = (Dvb_delsys)ddci_delsys;
				ad->post_init = (Adapter_commit)ddci_post_init;
				ad->close = (Adapter_commit)ddci_close;
				ad->get_signal = (Device_signal)NULL;
				ad->set_pid = (Set_pid)ddci_set_pid;
				ad->del_filters = (Del_filters)ddci_del_filters;
				ad->type = ADAPTER_DVB;

				ddci_adapters++;
				na++;
				a_count = na; // update adapter counter
				if (na == MAX_ADAPTERS)
					return;
				if (first_ddci == -1)
					first_ddci = na - 1;
#ifdef DDCI_TEST
				mapping_table_pids = ddci_adapters * PIDS_FOR_ADAPTER;
				mapping_table = malloc1(mapping_table_pids * sizeof(ddci_mapping_table_t));
				if (mapping_table)
					memset(mapping_table, -1, mapping_table_pids * sizeof(ddci_mapping_table_t));
				return;
#endif
			}
		}
	for (; na < MAX_ADAPTERS; na++)
		if (a[na])
			a[na]->pa = a[na]->fn = -1;

	mapping_table_pids = (ddci_adapters + 1) * PIDS_FOR_ADAPTER;
	mapping_table = malloc1(mapping_table_pids * sizeof(ddci_mapping_table_t));
	if (!mapping_table)
		LOG("could not allocate memory for mapping table");

	memset(mapping_table, -1, mapping_table_pids * sizeof(ddci_mapping_table_t));
}
