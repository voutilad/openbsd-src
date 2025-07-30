/*	$OpenBSD */

/*
 * Copyright (c) 2024 Mike Larkin <mlarkin@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bpfilter.h"
#include "vlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/device.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usbdevs.h>

#define URTWX_CTL_READ		1
#define URTWX_CTL_WRITE		2

#define URTWX_ENDPT_RX		0
#define URTWX_ENDPT_TX		1
#define URTWX_ENDPT_INTR	2
#define URTWX_ENDPT_MAX		3

#define URTWX_DEBUG 1

#ifdef URTWX_DEBUG
#define DPRINTF(x)	do { if (urtwxdebug) printf x; } while (0)
#define DPRINTFN(n,x)	do { if (urtwxdebug >= (n)) printf x; } while (0)
int	urtwxdebug = 9;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

struct urtwx_softc {
	struct device		 sc_dev;
	struct usbd_device	*sc_udev;

	struct usbd_interface	*sc_iface;
	struct usb_task		 sc_link_task;

	int			 sc_ed[URTWX_ENDPT_MAX];
	struct usbd_pipe	*sc_ep[URTWX_ENDPT_MAX];

	int			 sc_out_frame_size;
};

const struct usb_devno urtwx_devs[] = {
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8822BU },
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_1 },
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_1 },
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8822CU },
};

int		urtwx_match(struct device *, void *, void *);
void		urtwx_attach(struct device *, struct device *, void *);
int		urtwx_detach(struct device *, int);
int		urtwx_ctl(struct urtwx_softc *, uint8_t, uint8_t, uint16_t,
		    uint16_t, void *, int);

struct cfdriver urtwx_cd = {
	NULL, "urtwx", DV_IFNET
};

const struct cfattach urtwx_ca = {
	sizeof(struct urtwx_softc), urtwx_match, urtwx_attach, urtwx_detach
};

int
urtwx_ctl(struct urtwx_softc *sc, uint8_t rw, uint8_t cmd, uint16_t val,
    uint16_t index, void *buf, int len)
{
	usb_device_request_t	req;
	usbd_status		err;

	if (usbd_is_dying(sc->sc_udev))
		return 0;

	if (rw == URTWX_CTL_WRITE)
		req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	else
		req.bmRequestType = UT_READ_VENDOR_DEVICE;
	req.bRequest = cmd;
	USETW(req.wValue, val);
	USETW(req.wIndex, index);
	USETW(req.wLength, len);

	DPRINTFN(5, ("%s: rw %d, val 0x%04hx, index 0x%04hx, len %d\n",
	    __func__, rw, val, index, len));
	err = usbd_do_request(sc->sc_udev, &req, buf);
	if (err) {
		DPRINTF(("%s: error %d\n", __func__, err));
		return -1;
	}

	return 0;
}

int
urtwx_read_mem(struct urtwx_softc *sc, uint8_t cmd, uint16_t addr, uint16_t index,
    void *buf, int len)
{
	return (urtwx_ctl(sc, URTWX_CTL_READ, cmd, addr, index, buf, len));
}

int
urtwx_write_mem(struct urtwx_softc *sc, uint8_t cmd, uint16_t addr, uint16_t index,
    void *buf, int len)
{
	return (urtwx_ctl(sc, URTWX_CTL_WRITE, cmd, addr, index, buf, len));
}

uint8_t
urtwx_read_1(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint8_t		val;

	urtwx_read_mem(sc, cmd, reg, index, &val, 1);
	DPRINTFN(4, ("%s: cmd %x reg %x index %x = %x\n", __func__, cmd, reg,
	    index, val));
	return (val);
}

uint16_t
urtwx_read_2(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint16_t	val;

	urtwx_read_mem(sc, cmd, reg, index, &val, 2);
	DPRINTFN(4, ("%s: cmd %x reg %x index %x = %x\n", __func__, cmd, reg,
	    index, UGETW(&val)));

	return (UGETW(&val));
}

uint32_t
urtwx_read_4(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint32_t	val;

	urtwx_read_mem(sc, cmd, reg, index, &val, 4);
	DPRINTFN(4, ("%s: cmd %x reg %x index %x = %x\n", __func__, cmd, reg,
	    index, UGETDW(&val)));
	return (UGETDW(&val));
}

int
urtwx_write_1(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint8_t		temp;

	DPRINTFN(4, ("%s: cmd %x reg %x index %x: %x\n", __func__, cmd, reg,
	    index, val));
	temp = val & 0xff;
	return (urtwx_write_mem(sc, cmd, reg, index, &temp, 1));
}

int
urtwx_write_2(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint16_t	temp;

	DPRINTFN(4, ("%s: cmd %x reg %x index %x: %x\n", __func__, cmd, reg,
	    index, val));
	USETW(&temp, val & 0xffff);
	return (urtwx_write_mem(sc, cmd, reg, index, &temp, 2));
}

int
urtwx_write_4(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint8_t	temp[4];

	DPRINTFN(4, ("%s: cmd %x reg %x index %x: %x\n", __func__, cmd, reg,
	    index, val));
	USETDW(temp, val);
	return (urtwx_write_mem(sc, cmd, reg, index, &temp, 4));
}

int
urtwx_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg	*uaa = aux;

	if (uaa->iface == NULL || uaa->configno != 1)
		return (UMATCH_NONE);

	return (usb_lookup(urtwx_devs, uaa->vendor, uaa->product) != NULL ?
	    UMATCH_VENDOR_PRODUCT_CONF_IFACE : UMATCH_NONE);
}

void
urtwx_attach(struct device *parent, struct device *self, void *aux)
{
	struct urtwx_softc		*sc = (struct urtwx_softc *)self;
	struct usb_attach_arg		*uaa = aux;
	usb_interface_descriptor_t	*id;
	usb_endpoint_descriptor_t	*ed;
	int				i, s;

	sc->sc_udev = uaa->device;
	sc->sc_iface = uaa->iface;

	id = usbd_get_interface_descriptor(sc->sc_iface);

	for (i = 0; i < id->bNumEndpoints; i++) {
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (!ed) {
			printf("%s: couldn't get ep %d\n",
			    sc->sc_dev.dv_xname, i);
			return;
		}
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			sc->sc_ed[URTWX_ENDPT_RX] = ed->bEndpointAddress;
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			sc->sc_ed[URTWX_ENDPT_TX] = ed->bEndpointAddress;
			sc->sc_out_frame_size = UGETW(ed->wMaxPacketSize);
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
			sc->sc_ed[URTWX_ENDPT_INTR] = ed->bEndpointAddress;
		}
	}

	DPRINTFN(4, ("%s: tx endpoint=0x%x\n", __func__, sc->sc_ed[URTWX_ENDPT_TX]));
	DPRINTFN(4, ("%s: rx endpoint=0x%x\n", __func__, sc->sc_ed[URTWX_ENDPT_RX]));
	DPRINTFN(4, ("%s: intr endpoint=0x%x\n", __func__, sc->sc_ed[URTWX_ENDPT_INTR]));

	if ((sc->sc_ed[URTWX_ENDPT_RX] == 0) ||
	    (sc->sc_ed[URTWX_ENDPT_TX] == 0) ||
	    (sc->sc_ed[URTWX_ENDPT_INTR] == 0)) {
		printf("%s: missing one or more endpoints (%d, %d, %d)\n",
		    sc->sc_dev.dv_xname, sc->sc_ed[URTWX_ENDPT_RX],
		    sc->sc_ed[URTWX_ENDPT_TX], sc->sc_ed[URTWX_ENDPT_INTR]);
		return;
	}

	s = splnet();
#if 0
	printf("%s: ver %u.%u.%u", sc->sc_dev.dv_xname,
	    uaq_read_1(sc, UAQ_CMD_ACCESS_MAC, UAQ_FW_VER_MAJOR, 1) & 0x7f,
	    uaq_read_1(sc, UAQ_CMD_ACCESS_MAC, UAQ_FW_VER_MINOR, 1),
	    uaq_read_1(sc, UAQ_CMD_ACCESS_MAC, UAQ_FW_VER_REV, 1));

	uaq_read_mem(sc, UAQ_CMD_FLASH_PARAM, 0, 0, &sc->sc_ac.ac_enaddr,
	    ETHER_ADDR_LEN);
	printf(", address %s\n", ether_sprintf(sc->sc_ac.ac_enaddr));

	ifp = &sc->sc_ac.ac_if;
	ifp->if_softc = sc;
	strlcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = uaq_ioctl;
	ifp->if_start = uaq_start;
	ifp->if_watchdog = uaq_watchdog;

	ifp->if_capabilities = IFCAP_VLAN_MTU | IFCAP_CSUM_IPv4 |
	    IFCAP_CSUM_TCPv4 | IFCAP_CSUM_UDPv4;

#if NVLAN > 0
	ifp->if_capabilities |= IFCAP_VLAN_HWTAGGING;
#endif

	ifmedia_init(&sc->sc_ifmedia, IFM_IMASK, uaq_ifmedia_upd,
	    uaq_ifmedia_sts);
	uaq_add_media_types(sc);
	ifmedia_add(&sc->sc_ifmedia, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_ifmedia, IFM_ETHER | IFM_AUTO);
	sc->sc_ifmedia.ifm_media = sc->sc_ifmedia.ifm_cur->ifm_media;

	if_attach(ifp);
	ether_ifattach(ifp);
#endif
	splx(s);
}

int
urtwx_detach(struct device *self, int flags)
{
	struct urtwx_softc	*sc = (struct urtwx_softc *)self;
#if 0
	struct ifnet		*ifp = &sc->sc_ac.ac_if;
#endif
	int			s;

	DPRINTF(("%s: detaching\n", __func__));

	if (sc->sc_ep[URTWX_ENDPT_TX] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_TX]);
	if (sc->sc_ep[URTWX_ENDPT_RX] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_RX]);
	if (sc->sc_ep[URTWX_ENDPT_INTR] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_INTR]);

	s = splusb();

	usb_rem_task(sc->sc_udev, &sc->sc_link_task);

	usbd_ref_wait(sc->sc_udev);

	splx(s);

	DPRINTF(("%s: done\n", __func__));
	return 0;
}
