/*	$OpenBSD: ip_ahhmacsha1.c,v 1.4 1997/02/24 14:06:38 niklas Exp $	*/

/*
 * The author of this code is John Ioannidis, ji@tla.org,
 * 	(except when noted otherwise).
 *
 * This code was written for BSD/OS in Athens, Greece, in November 1995.
 *
 * Ported to OpenBSD and NetBSD, with additional transforms, in December 1996,
 * by Angelos D. Keromytis, kermit@forthnet.gr.
 *
 * Copyright (C) 1995, 1996, 1997 by John Ioannidis and Angelos D. Keromytis.
 *	
 * Permission to use, copy, and modify this software without fee
 * is hereby granted, provided that this entire notice is included in
 * all copies of any software which is or includes a copy or
 * modification of this software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTY. IN PARTICULAR, NEITHER AUTHOR MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE
 * MERCHANTABILITY OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR
 * PURPOSE.
 */

/*
 * Based on draft-ietf-ipsec-ah-hmac-sha-04.txt.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/socketvar.h>

#include <machine/cpu.h>
#include <machine/endian.h>

#include <net/if.h>
#include <net/route.h>
#include <net/netisr.h>
#include <net/raw_cb.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>

#include <net/encap.h>

#include <netinet/ip_ipsp.h>
#include <netinet/ip_ah.h>

/*
 * ahhmacsha1_attach() is called from the transformation initialization code.
 * It just returns.
 */

int
ahhmacsha1_attach()
{
	return 0;
}

/*
 * ahhmacsha1_init() is called when an SPI is being set up. It interprets the
 * encap_msghdr present in m, and sets up the transformation data.
 */

int
ahhmacsha1_init(struct tdb *tdbp, struct xformsw *xsp, struct mbuf *m)
{
	struct ahhmacsha1_xdata *xd;
	struct ahhmacsha1_xencap txd;
	struct encap_msghdr *em;
	int len;
	
	tdbp->tdb_xform = xsp;

	MALLOC(tdbp->tdb_xdata, caddr_t, sizeof (struct ahhmacsha1_xdata),
	       M_XDATA, M_WAITOK);
	if (tdbp->tdb_xdata == NULL)
	  return ENOBUFS;
	bzero(tdbp->tdb_xdata, sizeof (struct ahhmacsha1_xdata));
	bzero(&txd, sizeof(struct ahhmacsha1_xencap));
	xd = (struct ahhmacsha1_xdata *)tdbp->tdb_xdata;

	em = mtod(m, struct encap_msghdr *);
	if (em->em_msglen - EMT_SETSPI_FLEN > sizeof (struct ahhmacsha1_xencap))
	{
		free((caddr_t)tdbp->tdb_xdata, M_XDATA);
		tdbp->tdb_xdata = NULL;
		return EINVAL;
	}
	
	m_copydata(m, EMT_SETSPI_FLEN, em->em_msglen - EMT_SETSPI_FLEN, (caddr_t)&txd);

	xd->amx_rpl = 1;
	xd->amx_alen = txd.amx_alen;
	xd->amx_bitmap = 0;
	xd->amx_wnd = txd.amx_wnd;
	
	SHA1Init(&(xd->amx_ictx));
	SHA1Init(&(xd->amx_octx));
	
	for (len = 0; len < AHHMACSHA1_KMAX; len++)
	  txd.amx_key[len] ^= HMACSHA1_IPAD_VAL;

	SHA1Update(&(xd->amx_ictx), txd.amx_key, AHHMACSHA1_KMAX);

	for (len = 0; len < AHHMACSHA1_KMAX; len++)
	  txd.amx_key[len] ^= (HMACSHA1_IPAD_VAL ^ HMACSHA1_OPAD_VAL);

	SHA1Update(&(xd->amx_octx), txd.amx_key, AHHMACSHA1_KMAX);
	bzero(&txd, sizeof(struct ahhmacsha1_xencap));
	bzero(ipseczeroes, IPSEC_ZEROES_SIZE);	/* paranoid */

	return 0;
}

/*
 * Free memory
 */

int
ahhmacsha1_zeroize(struct tdb *tdbp)
{
	FREE(tdbp->tdb_xdata, M_XDATA);
	return 0;
}

/*
 * ahhmacsha1_input() gets called to verify that an input packet
 * passes authentication.
 */

extern struct ifnet loif;

struct mbuf *
ahhmacsha1_input(struct mbuf *m, struct tdb *tdb)
{
	struct ahhmacsha1_xdata *xd;
	struct ip *ip, ipo;
	struct ah *ah;
	struct ahhmacsha1 aho, *ahp;
	struct ifnet *rcvif;
	int ohlen, len, count, off, ado, errc;
	u_int64_t btsx;
	struct mbuf *m0;
	SHA1_CTX ctx; 
	
	xd = (struct ahhmacsha1_xdata *)tdb->tdb_xdata;
	ohlen = sizeof (struct ip) + AH_FLENGTH + xd->amx_alen;
	if (xd->amx_wnd >= 0)
	  ohlen += HMACSHA1_RPLENGTH;

	rcvif = m->m_pkthdr.rcvif;
	if (rcvif == NULL)
	{
#ifdef ENCDEBUG
		if (encdebug)
		  printf("ahhmacsha1_input: receive interface is NULL!!!\n");
#endif
		rcvif = &loif;
	}
	
	if (m->m_len < ohlen)
	{
		if ((m = m_pullup(m, ohlen)) == NULL)
		{
			ahstat.ahs_hdrops++;
			return NULL;
		}
	}

	ip = mtod(m, struct ip *);
	ah = (struct ah *)(ip + 1);
	ahp = (struct ahhmacsha1 *)ah;

        if (xd->amx_wnd >= 0)
          ado = HMACSHA1_RPLENGTH;
        else
          ado = 0;

        if (ah->ah_hl * sizeof(u_int32_t) != xd->amx_alen + ado)
        {
#ifdef ENCDEBUG
                if (encdebug)
                  printf("ahhmacsha1_input: bad authenticator length\n");
#endif
                ahstat.ahs_badauthl++;
                m_freem(m);
                return NULL;
        }

	ipo = *ip;
	ipo.ip_tos = 0;
	ipo.ip_len += sizeof (struct ip);	/* adjusted in ip_intr() */
	HTONS(ipo.ip_len);
	HTONS(ipo.ip_id);
	ipo.ip_off = htons(ipo.ip_off & IP_DF);	/* XXX -- and the C bit? */
	ipo.ip_ttl = 0;
	ipo.ip_sum = 0;

	ctx = xd->amx_ictx;
	SHA1Update(&ctx, (unsigned char *)&ipo, sizeof (struct ip));
	if (xd->amx_wnd >= 0)
	  SHA1Update(&ctx, (unsigned char *)ahp, 
		     AH_FLENGTH + HMACSHA1_RPLENGTH);
	else
	  SHA1Update(&ctx, (unsigned char *)ahp, AH_FLENGTH);
	SHA1Update(&ctx, ipseczeroes, xd->amx_alen);

	/*
	 * Code shamelessly stolen from m_copydata
	 */
	off = ohlen;
	len = m->m_pkthdr.len - off;
	m0 = m;
	
	while (off > 0)
	{
		if (m0 == 0)
		  panic("ahhmacsha1_input: m_copydata (off)");
		if (off < m0->m_len)
		  break;
		off -= m0->m_len;
		m0 = m0->m_next;
	}

	while (len > 0)
	{
		if (m0 == 0)
		  panic("ahhmacsha1_input: m_copydata (copy)");
		count = min(m0->m_len - off, len);
		SHA1Update(&ctx, mtod(m0, unsigned char *) + off, count);
		len -= count;
		off = 0;
		m0 = m0->m_next;
	}

	SHA1Final((unsigned char *)(&(aho.ah_data[0])), &ctx);
	ctx = xd->amx_octx;
	SHA1Update(&ctx, (unsigned char *)(&(aho.ah_data[0])), 
		   HMACSHA1_HASHLEN);
	SHA1Final((unsigned char *)(&(aho.ah_data[0])), &ctx);

	if (bcmp(aho.ah_data, ah->ah_data + ado, xd->amx_alen))
	{
#ifdef ENCDEBUG
		if (encdebug)
		  printf("ahhmacsha1_input: bad auth\n"); /* XXX */
#endif
		ahstat.ahs_badauth++;
		m_freem(m);
		return NULL;
	}
	
	if (xd->amx_wnd >= 0)
	{
	    	btsx = ntohq(ahp->ah_rpl);
		if ((errc = checkreplaywindow64(btsx, &(xd->amx_rpl), 
					       xd->amx_wnd, &(xd->amx_bitmap)))
		    != 0)
		{
			switch(errc)
			{
				case 1:
#ifdef ENCDEBUG
					printf("ahhmacsha1_input: replay counter wrapped\n");
#endif
					ahstat.ahs_wrap++;
					break;
				case 2:
#ifdef ENCDEBUG
					printf("ahhmacsha1_input: received old packet\n");
#endif
					ahstat.ahs_replay++;
					break;
				case 3:
#ifdef ENCDEBUG
					printf("ahhmacsha1_input: packet already received\n");
#endif
					ahstat.ahs_replay++;
					break;
			}
			m_freem(m);
			return NULL;
		}
	}
	
	ipo = *ip;
	ipo.ip_p = ah->ah_nh;

	m->m_len -= (ohlen - sizeof(struct ip));
	m->m_data += (ohlen - sizeof(struct ip));
	m->m_pkthdr.len -= (ohlen - sizeof(struct ip));
	m->m_pkthdr.rcvif = rcvif;	/* this should not be necessary */

	ip = mtod(m, struct ip *);
	*ip = ipo;
	ip->ip_len = htons(ip->ip_len - ohlen + 2 * sizeof (struct ip));
	HTONS(ip->ip_id);
	HTONS(ip->ip_off);
	ip->ip_sum = 0;
	ip->ip_sum = in_cksum(m, sizeof (struct ip));

	return m;
}


#define AHXPORT 

int
ahhmacsha1_output(struct mbuf *m, struct sockaddr_encap *gw, struct tdb *tdb, struct mbuf **mp)
{
	struct ahhmacsha1_xdata *xd;
	struct ip *ip, ipo;
	struct ah *ah;
	struct ahhmacsha1 *ahp, aho;
	register int len, off, count;
	register struct mbuf *m0;
	SHA1_CTX ctx;
	int ilen, ohlen;
	
	
	m = m_pullup(m, sizeof (struct ip));
	if (m == NULL)
	  return ENOBUFS;
	
	ip = mtod(m, struct ip *);
	
	xd = (struct ahhmacsha1_xdata *)tdb->tdb_xdata;

	ilen = ntohs(ip->ip_len);

#ifdef AHXPORT
	ohlen = AH_FLENGTH + xd->amx_alen;
#else
	ohlen = sizeof (struct ip) + AH_FLENGTH + xd->amx_alen;
#endif
	if (xd->amx_wnd >= 0)
	  ohlen += HMACSHA1_RPLENGTH;

	ipo.ip_v = IPVERSION;
	ipo.ip_hl = 5;
	ipo.ip_tos = 0;
	ipo.ip_len = htons(ohlen + ilen);
	ipo.ip_id = ip->ip_id;
	ipo.ip_off = htons(ntohs(ip->ip_off) & IP_DF);
	ipo.ip_ttl = 0;
	ipo.ip_p = IPPROTO_AH;
	ipo.ip_sum = 0;
#ifdef AHXPORT
	ipo.ip_src = ip->ip_src;
	ipo.ip_dst = ip->ip_dst;
	aho.ah_nh = ip->ip_p;
#else
	ipo.ip_src = gw->sen_ipsp_src;
	ipo.ip_dst = gw->sen_ipsp_dst;
	aho.ah_nh = IPPROTO_IP4;
#endif
	aho.ah_hl = (xd->amx_alen >> 2);
	if (xd->amx_wnd >= 0)
	  aho.ah_hl += (HMACSHA1_RPLENGTH / sizeof(u_int32_t));
	aho.ah_rv = 0;
	aho.ah_spi = tdb->tdb_spi;

	if (xd->amx_wnd >= 0)
	{
	    if (xd->amx_rpl == 0)
	    {
#ifdef ENCDEBUG
		printf("ahhmacsha1_output: key should have changed long ago\n");
#endif
		ahstat.ahs_wrap++;
		return NULL;
	    }
	}

        aho.ah_rpl = htonq(xd->amx_rpl++);

	ctx = xd->amx_ictx;
	SHA1Update(&ctx, (unsigned char *)&ipo, sizeof (struct ip));
	if (xd->amx_wnd >= 0)
	  SHA1Update(&ctx, (unsigned char *)&aho, 
		     AH_FLENGTH + HMACSHA1_RPLENGTH);
	else
	  SHA1Update(&ctx, (unsigned char *)&aho, AH_FLENGTH);
	SHA1Update(&ctx, ipseczeroes, xd->amx_alen);

#ifdef AHXPORT
	off = sizeof (struct ip);
#else	
	off = 0;
#endif

	/*
	 * Code shamelessly stolen from m_copydata
	 */
	len = m->m_pkthdr.len - off;
	
	m0 = m;

	while (len > 0)
	{
		if (m0 == 0)
		  panic("ahhmacsha1_output: m_copydata");
		count = min(m0->m_len - off, len);
		SHA1Update(&ctx, mtod(m0, unsigned char *) + off, count);

		len -= count;
		off = 0;
		m0 = m0->m_next;
	}

	SHA1Final((unsigned char *)(&(aho.ah_data[0])), &ctx);
	ctx = xd->amx_octx;
	SHA1Update(&ctx, (unsigned char *)(&(aho.ah_data[0])), 
		   HMACSHA1_HASHLEN);
	SHA1Final((unsigned char *)(&(aho.ah_data[0])), &ctx);

	ipo.ip_tos = ip->ip_tos;
	ipo.ip_id = ip->ip_id;
	ipo.ip_off = ip->ip_off;
	ipo.ip_ttl = ip->ip_ttl;
/*	ipo.ip_len = ntohs(ipo.ip_len); */
	
	M_PREPEND(m, ohlen, M_DONTWAIT);
	if (m == NULL)
	  return ENOBUFS;

	m = m_pullup(m, ohlen + sizeof (struct ip));
	if (m == NULL)
	  return ENOBUFS;
	
	ip = mtod(m, struct ip *);
	ah = (struct ah *)(ip + 1);
	ahp = (struct ahhmacsha1 *)ah; 
	*ip = ipo;
	ah->ah_nh = aho.ah_nh;
	ah->ah_hl = aho.ah_hl;
	ah->ah_rv = aho.ah_rv;
	ah->ah_spi = aho.ah_spi;
	if (xd->amx_wnd >= 0)
	{
	  	ahp->ah_rpl = aho.ah_rpl;
		bcopy((unsigned char *)(&(aho.ah_data[0])), 
		      ahp->ah_data, xd->amx_alen);
	}
	else
	  bcopy((unsigned char *)(&(aho.ah_data[0])), 
	        ah->ah_data, xd->amx_alen);

	*mp = m;
	
	return 0;
}
