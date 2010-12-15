/* ethernet.c -- Simulation of Ethernet MAC

   Copyright (C) 2001 by Erez Volk, erez@opencores.org
                         Ivan Guzvinec, ivang@opencores.org
   Copyright (C) 2008, 2001 Embecosm Limited
   Copyright (C) 2010 ORSoC

   Contributor Jeremy Bennett <jeremy.bennett@embecosm.com>
   Contributor Julius Baxter <julius@orsoc.se>

   This file is part of Or1ksim, the OpenRISC 1000 Architectural Simulator.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3 of the License, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   You should have received a copy of the GNU General Public License along
   with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* This program is commented throughout in a fashion suitable for processing
   with Doxygen. */


/* Autoconf and/or portability configuration */
#include "config.h"
#include "port.h"

/* System includes */
#include <stdlib.h>
#include <stdio.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <sys/poll.h>
#include <unistd.h>
#include <errno.h>

#include <linux/if.h>
#include <linux/if_tun.h>

/* Package includes */
#include "arch.h"
#include "config.h"
#include "abstract.h"
#include "eth.h"
#include "dma.h"
#include "sim-config.h"
#include "fields.h"
#include "crc32.h"
#include "vapi.h"
#include "pic.h"
#include "sched.h"
#include "toplevel-support.h"
#include "sim-cmd.h"


/* Control debug messages */
#define ETH_DEBUG 0
#ifndef ETH_DEBUG
# define ETH_DEBUG  1
#endif


/*! MAC address that is always accepted. */
static const unsigned char mac_broadcast[ETHER_ADDR_LEN] =
  { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* -------------------------------------------------------------------------- */
/*!Structure describing the Ethernet device                                   */
/* -------------------------------------------------------------------------- */
struct eth_device
{
  /* Basic stuff about the device */
  int                enabled;		/* Is peripheral enabled */
  oraddr_t           baseaddr;		/* Base address in memory */
  unsigned long int  base_vapi_id;	/* Start of VAPI ID block */

  /* DMA controller this MAC is connected to, and associated channels */
  unsigned dma;
  unsigned tx_channel;
  unsigned rx_channel;

  /* Details of the hardware */
  unsigned char      mac_address[ETHER_ADDR_LEN];  /* Ext HW address */
  unsigned long int  phy_addr;		/* Int HW address */
  unsigned long int  mac_int;		/* interrupt line number */
  int                int_line_stat;	/* interrupt line status */

  /* External interface deatils */
  int rtx_type;				/* Type of external i/f: FILE or TAP */

  /* RX and TX file names and handles for FILE type connection. */
  char  *rxfile;			/* Rx filename */
  char  *txfile;			/* Tx filename */
  int    txfd;				/* Rx file handle */
  int    rxfd;				/* Tx file handle */

  /* Info for TAP type connections */
  char *tap_dev;			/* The TAP device */
  int   rtx_fd;				/* TAP device handle */

  /* Indices into the buffer descriptors. */
  unsigned long int  tx_bd_index;
  unsigned long int  rx_bd_index;

  /* Visible registers */
  struct
  {
    unsigned long int  moder;
    unsigned long int  int_source;
    unsigned long int  int_mask;
    unsigned long int  ipgt;
    unsigned long int  ipgr1;
    unsigned long int  ipgr2;
    unsigned long int  packetlen;
    unsigned long int  collconf;
    unsigned long int  tx_bd_num;
    unsigned long int  controlmoder;
    unsigned long int  miimoder;
    unsigned long int  miicommand;
    unsigned long int  miiaddress;
    unsigned long int  miitx_data;
    unsigned long int  miirx_data;
    unsigned long int  miistatus;
    unsigned long int  hash0;
    unsigned long int  hash1;

    /* Buffer descriptors */
    unsigned long int  bd_ram[ETH_BD_SPACE / 4];
  } regs;
};



/* -------------------------------------------------------------------------- */
/*!Emulate MIIM transaction to ethernet PHY

   @param[in]  eth  Ethernet device datastruture.                           */
/* -------------------------------------------------------------------------- */
static void
eth_miim_trans (struct eth_device *eth)
{
  switch (eth->regs.miicommand)
    {
    case ((1 << ETH_MIICOMM_WCDATA_OFFSET)):
      /* Perhaps something to emulate here later, but for now do nothing */
      break;
      
    case ((1 << ETH_MIICOMM_RSTAT_OFFSET)):
      /*
      printf("or1ksim: eth_miim_trans: phy %d\n",(int)
	     ((eth->regs.miiaddress >> ETH_MIIADDR_FIAD_OFFSET)& 
	      ETH_MIIADDR_FIAD_MASK));
      printf("or1ksim: eth_miim_trans: reg %d\n",(int)
	     ((eth->regs.miiaddress >> ETH_MIIADDR_RGAD_OFFSET)&
	      ETH_MIIADDR_RGAD_MASK));
      */
      /*First check if it's the correct PHY to address */
      if (((eth->regs.miiaddress >> ETH_MIIADDR_FIAD_OFFSET)&
	   ETH_MIIADDR_FIAD_MASK) == eth->phy_addr)
	{
	  /* Correct PHY - now switch based on the register address in the PHY*/
	  switch ((eth->regs.miiaddress >> ETH_MIIADDR_RGAD_OFFSET)&
		  ETH_MIIADDR_RGAD_MASK)
	    {
	    case MII_BMCR:
	      eth->regs.miirx_data = BMCR_FULLDPLX;
	      break;
	    case MII_BMSR:
	      eth->regs.miirx_data = BMSR_LSTATUS | BMSR_ANEGCOMPLETE | 
		BMSR_10HALF | BMSR_10FULL | BMSR_100HALF | BMSR_100FULL;
	      break;
	    case MII_PHYSID1:
	      eth->regs.miirx_data = 0x22; /* Micrel PHYID */
	      break;
	    case MII_PHYSID2:
	      eth->regs.miirx_data = 0x1613; /* Micrel PHYID */
	      break;
	    case MII_ADVERTISE:
	      eth->regs.miirx_data = ADVERTISE_FULL;
	      break;
	    case MII_LPA:
	      eth->regs.miirx_data = LPA_DUPLEX | LPA_100;
	      break;
	    case MII_EXPANSION:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_CTRL1000:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_STAT1000:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_ESTATUS:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_DCOUNTER:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_FCSCOUNTER:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_NWAYTEST:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_RERRCOUNTER:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_SREVISION:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_RESV1:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_LBRERROR:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_PHYADDR:
	      eth->regs.miirx_data = eth->phy_addr;
	      break;
	    case MII_RESV2:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_TPISTATUS:
	      eth->regs.miirx_data = 0;
	      break;
	    case MII_NCONFIG:
	      eth->regs.miirx_data = 0;
	      break;
	    default:
	      eth->regs.miirx_data = 0xffff;
	      break;
	    }
	}
      else
	{
	  eth->regs.miirx_data = 0xffff; /* PHY doesn't exist, read all 1's */
	}

      break;

    case ((1 << ETH_MIICOMM_SCANS_OFFSET)):
      /* From MAC's datasheet: 
	 A host initiates the Scan Status Operation by asserting the SCANSTAT 
	 signal. The MIIM performs a continuous read operation of the PHY 
	 Status register. The PHY is selected by the FIAD[4:0] signals. The 
	 link status LinkFail signal is asserted/deasserted by the MIIM module 
	 and reflects the link status bit of the PHY Status register. The 
	 signal NVALID is used for qualifying the validity of the LinkFail 
	 signals and the status data PRSD[15:0]. These signals are invalid 
	 until the first scan status operation ends. During the scan status 
	 operation, the BUSY signal is asserted until the last read is 
	 performed (the scan status operation is stopped).

	 So for now - do nothing, leave link status indicator as permanently 
	 with link.
      */

      break;
      
    default:
      break;      
    }
}	/* eth_miim_trans () */



/* -------------------------------------------------------------------------- */
/*!Write an Ethernet packet to a FILE interface.

   This writes a single Ethernet packet to a FILE interface. The format is to
   write the length, then the data.

   @param[in] eth     Pointer to the relevant Ethernet data structure.
   @param[in] buf     Where to get the data.
   @param[in] length  Length of data to write.

   @return  The length if successful, a negative value otherwise.             */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_write_file_packet (struct eth_device *eth,
		       unsigned char     *buf,
		       unsigned long int  length)
{
  ssize_t  nwritten;

  /* Write length to file. */
  nwritten = write (eth->txfd, &(length), sizeof (length));
  if (nwritten != sizeof (length))
    {
      fprintf (stderr, "ERROR: Failed to write Ethernet packet length: %s.\n",
	       strerror (errno));
      return  -1;
    }

  /* write data to file */
  nwritten = write (eth->txfd, buf, length);
  if (nwritten != length)
    {
      fprintf (stderr, "ERROR: Failed to write Ethernet packet data: %s.\n",
	       strerror (errno));
      return  -1;
    }

  return  nwritten;

}	/* eth_write_file_packet () */


/* -------------------------------------------------------------------------- */
/*!Write an Ethernet packet to a TAP interface.

   This writes a single Ethernet packet to a TAP interface.

   @param[in] eth     Pointer to the relevant Ethernet data structure.
   @param[in] buf     Where to get the data.
   @param[in] length  Length of data to write.

   @return  The length if successful, a negative value otherwise.             */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_write_tap_packet (struct eth_device *eth,
		      unsigned char     *buf,
		      unsigned long int  length)
{
  ssize_t  nwritten;

#if ETH_DEBUG
  int  j; 

  printf ("Writing TAP\n");
  printf ("  packet %d bytes:", (int) length);

  for (j = 0; j < length; j++)
    {
      if (0 == (j % 16))
	{
	  printf ("\n");
	}
      else if (0 == (j % 8))
	{
	  printf (" ");
	}
      
      printf ("%.2x ", buf[j]);
    }
  
  printf("\nend packet:\n");
#endif	  

  /* Write the data to the TAP */
  nwritten = write (eth->rtx_fd, buf, length);
  if (nwritten != length)
    {
      fprintf (stderr, "ERROR: Failed to write Ethernet packet data: %s.\n",
	       strerror (errno));
      return  -1;
    }

  return  nwritten;

}	/* eth_write_tap_packet () */


/* -------------------------------------------------------------------------- */
/*!Write an Ethernet packet.

   This writes a single Ethernet packet to the outside world from the supplied
   buffer. It deals with the different types of external interface.

   @param[in] eth     Pointer to the relevant Ethernet data structure.
   @param[in] buf     Where to get the data.
   @param[in] length  Length of data to write.

   @return  The length if successful, zero if no packet was available,
            a negative value otherwise.                                       */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_write_packet (struct eth_device *eth,
		  unsigned char     *buf,
		  ssize_t            length)
{
  /* Send packet according to interface type. */
  switch (eth->rtx_type)
    {
    case ETH_RTX_FILE: return  eth_write_file_packet (eth, buf, length);
    case ETH_RTX_TAP:  return  eth_write_tap_packet (eth, buf, length);

    default:
      fprintf (stderr, "Unknown Ethernet write interface: ignored.\n");
      return  (ssize_t) -1;
    }
}	/* eth_write_packet () */


/* -------------------------------------------------------------------------- */
/*!Flush a Tx buffer descriptor to the outside world.

   We know the buffer descriptor is full, so write it to the appropriate
   outside interface.

   @param[in] eth  The Ethernet data structure.                               */
/* -------------------------------------------------------------------------- */
static void
eth_flush_bd (struct eth_device *eth)
{
  /* First word of BD is flags and length, second is pointer to buffer */
  unsigned long int  bd_info = eth->regs.bd_ram[eth->tx_bd_index];
  unsigned long int  bd_addr = eth->regs.bd_ram[eth->tx_bd_index + 1];
  unsigned char      buf[ETH_MAXPL];
  long int           packet_length;
  long int           bytes_sent;

  /* Get the packet length */
  packet_length = GET_FIELD (bd_info, ETH_TX_BD, LENGTH);

  /* Clear error status bits and retry count. */
  CLEAR_FLAG (bd_info, ETH_TX_BD, DEFER);
  CLEAR_FLAG (bd_info, ETH_TX_BD, COLLISION);
  CLEAR_FLAG (bd_info, ETH_TX_BD, RETRANSMIT);
  CLEAR_FLAG (bd_info, ETH_TX_BD, UNDERRUN);
  CLEAR_FLAG (bd_info, ETH_TX_BD, NO_CARRIER);

  SET_FIELD (bd_info, ETH_TX_BD, RETRY, 0);

  /* Copy data from buffer descriptor address into our local buf. */
  for (bytes_sent = 0; bytes_sent < packet_length; bytes_sent +=4)
    {
      unsigned long int  read_word =
	eval_direct32 (bytes_sent + bd_addr, 0, 0);

      buf[bytes_sent]     = (unsigned char) (read_word >> 24);
      buf[bytes_sent + 1] = (unsigned char) (read_word >> 16);
      buf[bytes_sent + 2] = (unsigned char) (read_word >> 8);
      buf[bytes_sent + 3] = (unsigned char) (read_word);
    }

  /* Send packet according to interface type and set BD status. If we didn't
     write the whole packet, then we retry. */
  if (eth_write_packet (eth, buf, packet_length) == packet_length)
    {
      CLEAR_FLAG (bd_info, ETH_TX_BD, READY);
      SET_FLAG (eth->regs.int_source, ETH_INT_SOURCE, TXB);
    }
  else
    {
      /* Does this retry mechanism really work? */
      CLEAR_FLAG (bd_info, ETH_TX_BD, READY);
      CLEAR_FLAG (bd_info, ETH_TX_BD, COLLISION);
      SET_FLAG (eth->regs.int_source, ETH_INT_SOURCE, TXE);
#if ETH_DEBUG
      printf ("Transmit retry request.\n");
#endif
    }

  /* Update the flags in the buffer descriptor */
  eth->regs.bd_ram[eth->tx_bd_index] = bd_info;

  /* Generate interrupt to indicate transfer complete, under the
     following criteria all being met:
     - either INT_MASK flag for Tx (OK or error) is set
     - the bugger descriptor has its IRQ flag set
     - there is no interrupt in progress.

     @todo We ought to warn if we get here and fail to set an IRQ. */
  if ((TEST_FLAG (eth->regs.int_mask, ETH_INT_MASK, TXE_M) ||
       TEST_FLAG (eth->regs.int_mask, ETH_INT_MASK, TXB_M)) &&
      TEST_FLAG (bd_info, ETH_TX_BD, IRQ))
    {
      if (eth->int_line_stat)
	{
	  fprintf (stderr, "Warning: Interrupt active during Tx.\n");
	}
      else
	{
#if ETH_DEBUG
	  printf ("TRANSMIT interrupt\n");
#endif
	  report_interrupt (eth->mac_int);
	  eth->int_line_stat = 1;
	}
    }

  /* Advance to next BD, wrapping around if appropriate. */
  if (TEST_FLAG (bd_info, ETH_TX_BD, WRAP) ||
      eth->tx_bd_index >= ((eth->regs.tx_bd_num - 1) * 2))
    {
      eth->tx_bd_index = 0;
    }
  else
    {
      eth->tx_bd_index += 2;
    }
}	/* eth_flush_bd () */


/* -------------------------------------------------------------------------- */
/*!Tx clock function.

   Responsible for starting and completing any TX actions.

   The original version had 4 states, which allowed modeling the transfer of
   data one byte per cycle.  For now we use only the one state for
   efficiency. When we find something in a buffer descriptor, we transmit
   it.

   We reschedule every cycle. There is no point in trying to do anything if
   there is an interrupt still being processed by the core.

   @todo We should eventually reinstate the one byte per cycle transfer.

   @param[in] dat  The Ethernet data structure, passed as a void pointer.    */
/* -------------------------------------------------------------------------- */
static void
eth_controller_tx_clock (void *dat)
{
  struct eth_device *eth = dat;

  /* Only do anything if there is not an interrupt outstanding. */
  if (!eth->int_line_stat)
    {
      /* First word of BD is flags. If we have a buffer ready, get it and
	 transmit it. */
      if (TEST_FLAG (eth->regs.bd_ram[eth->tx_bd_index], ETH_TX_BD, READY))
	{
	  eth_flush_bd (eth);
	}
    }

  /* Wake up again after 1 ticks (was 10, changed by Julius). */
  SCHED_ADD (eth_controller_tx_clock, dat, 1);

}	/* eth_controller_tx_clock () */


/* -------------------------------------------------------------------------- */
/*!Read an Ethernet packet from a FILE interface.

   This reads a single Ethernet packet from the outside world via a FILE
   interface.

   The format is 4 bytes of packet length, followed by the packet data.

   @param[in]  eth  Pointer to the relevant Ethernet data structure
   @param[out] buf  Where to put the data

   @return  The length if successful, zero if no packet was available
            (i.e. EOF), a negative value otherwise.                           */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_read_file_packet (struct eth_device *eth,
		      unsigned char     *buf)
{
  ssize_t  packet_length;
  ssize_t  nread;

  /* Read packet length. We may be at EOF. */
  nread = read (eth->rxfd, &(packet_length), sizeof (packet_length));

  if (0 == nread)
    {
      return  0;			/* No more packets */
    }
  else if (nread < sizeof (packet_length))
    {
      fprintf (stderr, "ERROR: Failed to read length from file.\n");
      return  -1;
    }

  /* Packet must be big enough to hold a header */
  if (packet_length < ETHER_HDR_LEN)
    {
      fprintf (stderr, "Warning: Ethernet packet length %zd too small.\n",
	       packet_length);
      return  -1;
    }

  /* Read the packet proper. */
  nread = read (eth->rxfd, buf, packet_length);

  if (nread != packet_length)
    {
      fprintf (stderr, "ERROR: Failed to read packet from file.\n");
      return  -1;
    }

  return  packet_length;

}	/* eth_read_file_packet () */


/* -------------------------------------------------------------------------- */
/*!Read an Ethernet packet from a FILE interface.

   This reads a single Ethernet packet from the outside world via a TAP
   interface.

   A complete packet is always read, so its length (minus CRC) is the amount
   read.

   @param[in]  eth  Pointer to the relevant Ethernet data structure
   @param[out] buf  Where to put the data

   @return  The length if successful, zero if no packet was available,
            a negative value otherwise.                                       */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_read_tap_packet (struct eth_device *eth,
		     unsigned char     *buf)
{
  struct pollfd  fds[1];
  int            n;
  ssize_t        packet_length;

  /* Poll to see if there is data to read */
  fds[0].fd     = eth->rtx_fd;
  fds[0].events = POLLIN;
 
  n = poll (fds, 1, 0);
  if (n < 0)
    {
      fprintf (stderr, "Warning: Poll for TAP receive failed %s: ignored.\n",
	       strerror (errno));
      return  -1;
    }
  else if ((n > 0) && ((fds[0].revents & POLLIN) == POLLIN))
    {
      /* Data to be read from TAP */
      packet_length = read (eth->rtx_fd, buf, ETH_MAXPL);
#if ETH_DEBUG
      printf ("%d bytes read from TAP.\n", (int) packet_length);
#endif
      if (packet_length < 0)
	{
	  fprintf (stderr, "Warning: Read of RXTATE_RECV failed: %s.\n",
		   strerror (errno));		  		  
	  SET_FLAG (eth->regs.int_source, ETH_INT_SOURCE, RXE);

	  /* Signal interrupt if enabled, and no interrupt currently in
	     progress. */
	  if (TEST_FLAG (eth->regs.int_mask, ETH_INT_MASK, RXE_M) &&
	      !eth->int_line_stat)
	    {		      
#if ETH_DEBUG
	      printf ("Ethernet failed receive interrupt\n");
#endif
	      report_interrupt (eth->mac_int);
	      eth->int_line_stat = 1;
	    }
	}
	
      return  packet_length;
    }
  else
    {
      return  0;			/* No packet */
    }
}	/* eth_read_tap_packet () */


/* -------------------------------------------------------------------------- */
/*!Read an Ethernet packet.

   This reads a single Ethernet packet from the outside world into the
   supplied buffer. It deals with the different types of external interface.

   @param[in]  eth  Pointer to the relevant Ethernet data structure
   @param[out] buf  Where to put the data

   @return  The length if successful, zero if no packet was available,
            a negative value otherwise.                                       */
/* -------------------------------------------------------------------------- */
static ssize_t
eth_read_packet (struct eth_device *eth,
		 unsigned char     *buf)
{
  switch (eth->rtx_type)
    {
    case ETH_RTX_FILE: return  eth_read_file_packet (eth, buf);
    case ETH_RTX_TAP:  return  eth_read_tap_packet (eth, buf);

    default:
      fprintf (stderr, "Unknown Ethernet read interface: ignored.\n");
      return  (ssize_t) -1;
    }
}	/* eth_read_packet () */


/* -------------------------------------------------------------------------- */
/*!Fill a buffer descriptor

   A buffer descriptor is empty. Attempt to fill it from the outside world.

   @param[in] eth  The Ethernet data structure, passed as a void pointer.    */
/* -------------------------------------------------------------------------- */
static void
eth_fill_bd (struct eth_device *eth)
{

  /* First word of BD is flags and length, second is pointer to buffer */
  unsigned long int  bd_info = eth->regs.bd_ram[eth->rx_bd_index];
  unsigned long int  bd_addr = eth->regs.bd_ram[eth->rx_bd_index + 1];

  long int           packet_length;
  long int           bytes_read;
  unsigned char      buf[ETH_MAXPL];

  /* Clear various status bits */
  CLEAR_FLAG (bd_info, ETH_RX_BD, MISS);
  CLEAR_FLAG (bd_info, ETH_RX_BD, INVALID);
  CLEAR_FLAG (bd_info, ETH_RX_BD, DRIBBLE);
  CLEAR_FLAG (bd_info, ETH_RX_BD, UVERRUN);
  CLEAR_FLAG (bd_info, ETH_RX_BD, COLLISION);
  CLEAR_FLAG (bd_info, ETH_RX_BD, TOOBIG);
  CLEAR_FLAG (bd_info, ETH_RX_BD, TOOSHORT);

  /* Loopback is permitted. We believe that Linux never uses it, so we'll
     note the attempt and ignore.
     
     @todo We should support this. */
  if (TEST_FLAG (eth->regs.moder, ETH_MODER, LOOPBCK))
    {
      PRINTF ("Ethernet loopback requested.\n");
      fprintf (stderr, "ERROR: Loopback not supported. Ignored.\n");
    }
  
  packet_length = eth_read_packet (eth, buf);
  if (packet_length <= 0)
    {
      /* Empty packet or error. No more to do here. */
      return;
    }
  
/* Got a packet successfully. If not promiscuous mode, check the destination
   address is meant for us. */
  if (!TEST_FLAG (eth->regs.moder, ETH_MODER, PRO))
    {
      if (TEST_FLAG (eth->regs.moder, ETH_MODER, IAM))
	{
	  /* There is little documentation of how IAM is supposed to work. It
	     seems that some mapping function (not defined) maps the address
	     down to a number in the range 0-63. If that bit is set in
	     HASH0/HASH1 registers, the packet is accepted. */
	  fprintf (stderr, "Warning: Individual Address Mode ignored.\n");
	}
	      
      /* Check for HW address match. */
      if ((0 != bcmp (eth->mac_address, buf, ETHER_ADDR_LEN)) &&
	  (0 != bcmp (mac_broadcast,    buf, ETHER_ADDR_LEN)))
	{
#if ETH_DEBUG		
	  printf ("packet for %.2x:%.2x:%.2x:%.2x:%.2x:%.2x ignored.\n",
		  buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
#endif
	  /* Not for us. No more to do here. */
	  return;
	}
    }

  /* Transfer the buffer into the BD. */
#if ETH_DEBUG
  printf ("writing to Rx BD%d: %d bytes @ 0x%.8x\n",
	  (int) eth->rx_bd_index / 2,  (int) packet_length, 
	  (unsigned int)bd_addr);
#endif
	  
  for (bytes_read = 0; bytes_read < packet_length; bytes_read +=4)
    {
      unsigned long int  send_word =
	((unsigned long) buf[bytes_read]     << 24) |
	((unsigned long) buf[bytes_read + 1] << 16) |
	((unsigned long) buf[bytes_read + 2] <<  8) |
	((unsigned long) buf[bytes_read + 3]      );
      set_direct32 (bd_addr + bytes_read, send_word, 0, 0);
    }
  
#if ETH_DEBUG
  printf("BD filled with 0x%08lx bytes.\n", bytes_read);
#endif
	  
  /* Write result to BD.
	     
     @todo Why is the length 4 more than the packet length? Is that for
     the CRC? */
  SET_FIELD (bd_info, ETH_RX_BD, LENGTH, packet_length + 4);
  CLEAR_FLAG (bd_info, ETH_RX_BD, READY);
  SET_FLAG (eth->regs.int_source, ETH_INT_SOURCE, RXB);
      
  eth->regs.bd_ram[eth->rx_bd_index] = bd_info;
	  
  /* Advance to next BD. The Rx BDs start after the Tx BDs. */
  if (TEST_FLAG (bd_info, ETH_RX_BD, WRAP) ||
      (eth->rx_bd_index >= ETH_BD_COUNT))
    {
      eth->rx_bd_index = eth->regs.tx_bd_num * 2;
    }
  else
    {
      eth->rx_bd_index += 2;
    }
  
  /* Raise an interrupt if necessary. */
  if (TEST_FLAG (eth->regs.int_mask, ETH_INT_MASK, RXB_M) &&
      TEST_FLAG (bd_info, ETH_RX_BD, IRQ))
    {
      if (eth->int_line_stat)
	{
	  fprintf (stderr, "Warning: Interrupt active during Rx.\n");
	}
      else
	{
#if ETH_DEBUG
	  printf ("Rx successful receive interrupt\n");
#endif
	  report_interrupt (eth->mac_int);
	  eth->int_line_stat = 1;
	}
    }
}	/* eth_fill_bd () */


/* -------------------------------------------------------------------------- */
/*!Ignore a packet from the outside world.

   We don't have a BD ready, so any packets waiting should be thrown away.

   @param[in] eth  The Ethernet data structure.                               */
/* -------------------------------------------------------------------------- */
static void
eth_ignore_packet (struct eth_device *eth)
{
  unsigned char  buf[ETH_MAXPL];
  ssize_t        nread = eth_read_packet (eth, buf);

  if (nread < 0)
    {
      fprintf (stderr,
	       "Warning: Read of when Ethernet busy failed %s.\n",
	       strerror (errno));
    }
  else if (nread > 0)
    {
      /* Record that a packet was thrown away. */
      SET_FLAG (eth->regs.int_source, ETH_INT_SOURCE, BUSY);
      PRINTF ("Ethernet discarding %d bytes from TAP while BD full.\n",
	      nread);
      
      /* Raise an interrupt if necessary. */
      if (TEST_FLAG (eth->regs.int_mask, ETH_INT_MASK, BUSY_M))
	{
	  if (eth->int_line_stat)
	    {
	      fprintf (stderr, "Warning: Interrupt active during ignore.\n");
	    }
	  else
	    {
#if ETH_DEBUG
	      printf ("Ethernet Rx BUSY interrupt\n");
#endif
	      report_interrupt (eth->mac_int);
	      eth->int_line_stat = 1;
	    }
	}
    }
}	/* eth_ignore_packet () */


/* -------------------------------------------------------------------------- */
/*!Rx clock function.

   Responsible for starting and completing any RX actions.

   The original version had 4 states, which allowed modeling the transfer of
   data one byte per cycle.  For now we use only the one state for
   efficiency. When the buffer is empty, we fill it from the external world.

   We schedule to wake up again each cycle. This means we will get called when
   the core is still processing the previous interrupt. To avoid races, we do
   nothing until the interrupt is cleared.

   @todo We should eventually reinstate the one byte per cycle transfer.

   @param[in] dat  The Ethernet data structure, passed as a void pointer.    */
/* -------------------------------------------------------------------------- */
static void
eth_controller_rx_clock (void *dat)
{
  struct eth_device *eth = dat;

  /* Only do anything if there is not an interrupt outstanding. */
  if (!eth->int_line_stat)
    {
      /* First word of the BD is flags, where we can test if it's ready. */
      if (TEST_FLAG (eth->regs.bd_ram[eth->rx_bd_index], ETH_RX_BD, READY))
	{
	  /* The BD is empty, so we try to fill it with data from the outside
	     world. */
	  eth_fill_bd (eth);	/* BD ready to be filled. */
	}
      else if ((TEST_FLAG (eth->regs.moder, ETH_MODER, RXEN)) &&
	       (ETH_RTX_FILE == eth->rtx_type))
	{
	  /* The BD is full, Rx is enabled and we are reading from an external
	     TAP interface. We can't take any more, so we'll throw oustanding
	     input packets on the floor.
	     
	     @note We don't do this for file I/O, since it would discard
	     everything immediately! */
	  eth_ignore_packet (eth);
	}
    }

  /* Whatever happens, we reschedule a wake up in the future. This used to be
     every 10 ticks, but now it is very 1 tick. */
  SCHED_ADD (eth_controller_rx_clock, dat, 1);

}	/* eth_controller_rx_clock () */


/* ========================================================================= */

/* ========================================================================= */


/*
 *   VAPI connection to outside
 */
static void
eth_vapi_read (unsigned long id, unsigned long data, void *dat)
{
  unsigned long which;
  struct eth_device *eth = dat;

  which = id - eth->base_vapi_id;

  if (!eth)
    {
      return;
    }

  switch (which)
    {
    case ETH_VAPI_DATA:
      break;
    case ETH_VAPI_CTRL:
      break;
    }
}

/* -------------------------------------------------------------------------- */
/*!Reset the Ethernet.

   Open the correct type of simulation interface to the outside world.

   Initialize all registers to default and places devices in memory address
   space.

   @param[in] dat  The Ethernet interface data structure.                     */
/* -------------------------------------------------------------------------- */
static void
eth_reset (void *dat)
{
  struct eth_device *eth = dat;
  struct ifreq       ifr;

#if ETH_DEBUG
  printf ("Resetting Ethernet\n");
#endif
  /* Nothing to do if we do not have a base address set.

     TODO: Surely this should test for being enabled? */
  if (0 == eth->baseaddr)
    {
      return;
    }

  switch (eth->rtx_type)
    {
    case ETH_RTX_FILE:

      /* (Re-)open TX/RX files */
      if (eth->rxfd >= 0)
	{
	  close (eth->rxfd);
	}

      if (eth->txfd >= 0)
	{
	  close (eth->txfd);
	}

      eth->rxfd = -1;
      eth->txfd = -1;

      eth->rxfd = open (eth->rxfile, O_RDONLY);
      if (eth->rxfd < 0)
	{
	  fprintf (stderr, "Warning: Cannot open Ethernet RX file \"%s\": %s\n",
		   eth->rxfile, strerror (errno));
	}

      eth->txfd = open (eth->txfile,
#if defined(O_SYNC)		/* BSD/MacOS X doesn't know about O_SYNC */
			O_SYNC |
#endif
			O_RDWR | O_CREAT | O_APPEND,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
      if (eth->txfd < 0)
	{
	  fprintf (stderr, "Warning: Cannot open Ethernet TX file \"%s\": %s\n",
		   eth->txfile, strerror (errno));
	}

      break;

    case ETH_RTX_TAP:

      /* (Re-)open TAP interface if necessary */
      if (eth->rtx_fd != 0)
	{
	  break;
	}

      /* Open the TUN/TAP device */
      eth->rtx_fd = open ("/dev/net/tun", O_RDWR);
      if( eth->rtx_fd < 0 )
	{
	  fprintf (stderr, "Warning: Failed to open TUN/TAP device: %s\n",
		   strerror (errno));
	  eth->rtx_fd = 0;
	  return;
	}

      /* Turn it into a specific TAP device. If we haven't specified a
	 specific (persistent) device, one will be created, but that requires
	 superuser, or at least CAP_NET_ADMIN capabilities. */
      memset (&ifr, 0, sizeof(ifr));
      ifr.ifr_flags = IFF_TAP | IFF_NO_PI; 
      strncpy (ifr.ifr_name, eth->tap_dev, IFNAMSIZ);

      if (ioctl (eth->rtx_fd, TUNSETIFF, (void *) &ifr) < 0)
	{
	  fprintf (stderr, "Warning: Failed to set TAP device: %s\n",
		   strerror (errno));
	  close (eth->rtx_fd);
	  eth->rtx_fd = 0;
	  return;
	}
#if ETH_DEBUG
      PRINTF ("Opened TAP %s\n", ifr.ifr_name);
#endif
      /* Do we need to flush any packets? */
      break;
    }

  /* Set registers to default values */
  memset (&(eth->regs), 0, sizeof (eth->regs));

  eth->regs.moder     = 0x0000A000;
  eth->regs.ipgt      = 0x00000012;
  eth->regs.ipgr1     = 0x0000000C;
  eth->regs.ipgr2     = 0x00000012;
  eth->regs.packetlen = 0x003C0600;
  eth->regs.collconf  = 0x000F003F;
  eth->regs.miimoder  = 0x00000064;
  eth->regs.tx_bd_num = 0x00000040;

  /* Reset TX/RX BD indexes. The Rx BD indexes start after the Tx BD indexes. */
  eth->tx_bd_index = 0;
  eth->rx_bd_index = eth->regs.tx_bd_num * 2;

  /* Reset IRQ line status */
  eth->int_line_stat = 0;

  /* Initialize VAPI */
  if (eth->base_vapi_id)
    {
      vapi_install_multi_handler (eth->base_vapi_id, ETH_NUM_VAPI_IDS,
				  eth_vapi_read, dat);
    }
}	/* eth_reset () */


/* 
  Print register values on stdout 
*/
static void
eth_status (void *dat)
{
  struct eth_device *eth = dat;

  PRINTF ("\nEthernet MAC at 0x%" PRIxADDR ":\n", eth->baseaddr);
  PRINTF ("MODER        : 0x%08lX\n", eth->regs.moder);
  PRINTF ("INT_SOURCE   : 0x%08lX\n", eth->regs.int_source);
  PRINTF ("INT_MASK     : 0x%08lX\n", eth->regs.int_mask);
  PRINTF ("IPGT         : 0x%08lX\n", eth->regs.ipgt);
  PRINTF ("IPGR1        : 0x%08lX\n", eth->regs.ipgr1);
  PRINTF ("IPGR2        : 0x%08lX\n", eth->regs.ipgr2);
  PRINTF ("PACKETLEN    : 0x%08lX\n", eth->regs.packetlen);
  PRINTF ("COLLCONF     : 0x%08lX\n", eth->regs.collconf);
  PRINTF ("TX_BD_NUM    : 0x%08lX\n", eth->regs.tx_bd_num);
  PRINTF ("CTRLMODER    : 0x%08lX\n", eth->regs.controlmoder);
  PRINTF ("MIIMODER     : 0x%08lX\n", eth->regs.miimoder);
  PRINTF ("MIICOMMAND   : 0x%08lX\n", eth->regs.miicommand);
  PRINTF ("MIIADDRESS   : 0x%08lX\n", eth->regs.miiaddress);
  PRINTF ("MIITX_DATA   : 0x%08lX\n", eth->regs.miitx_data);
  PRINTF ("MIIRX_DATA   : 0x%08lX\n", eth->regs.miirx_data);
  PRINTF ("MIISTATUS    : 0x%08lX\n", eth->regs.miistatus);
  PRINTF ("MAC Address  : %02X:%02X:%02X:%02X:%02X:%02X\n",
	  eth->mac_address[5], eth->mac_address[4], eth->mac_address[3],
	  eth->mac_address[2], eth->mac_address[1], eth->mac_address[0]);
  PRINTF ("HASH0        : 0x%08lX\n", eth->regs.hash0);
  PRINTF ("HASH1        : 0x%08lX\n", eth->regs.hash1);
}

/* ========================================================================= */


/* 
  Read a register 
*/
static uint32_t
eth_read32 (oraddr_t addr, void *dat)
{
  struct eth_device *eth = dat;

  switch (addr)
    {
    case ETH_MODER:
      return eth->regs.moder;
    case ETH_INT_SOURCE:
      return eth->regs.int_source;
    case ETH_INT_MASK:
      return eth->regs.int_mask;
    case ETH_IPGT:
      return eth->regs.ipgt;
    case ETH_IPGR1:
      return eth->regs.ipgr1;
    case ETH_IPGR2:
      return eth->regs.ipgr2;
    case ETH_PACKETLEN:
      return eth->regs.packetlen;
    case ETH_COLLCONF:
      return eth->regs.collconf;
    case ETH_TX_BD_NUM:
      return eth->regs.tx_bd_num;
    case ETH_CTRLMODER:
      return eth->regs.controlmoder;
    case ETH_MIIMODER:
      return eth->regs.miimoder;
    case ETH_MIICOMMAND:
      return eth->regs.miicommand;
    case ETH_MIIADDRESS:
      return eth->regs.miiaddress;
    case ETH_MIITX_DATA:
      return eth->regs.miitx_data;
    case ETH_MIIRX_DATA:
      /*printf("or1ksim: read MIIM RX: 0x%x\n",(int)eth->regs.miirx_data);*/
      return eth->regs.miirx_data;
    case ETH_MIISTATUS:
      return eth->regs.miistatus;
    case ETH_MAC_ADDR0:
      return (((unsigned long) eth->mac_address[2]) << 24) |
	(((unsigned long) eth->mac_address[3]) << 16) |
	(((unsigned long) eth->mac_address[4]) << 8) |
	(unsigned long) eth->mac_address[5];
    case ETH_MAC_ADDR1:
      return (((unsigned long) eth->mac_address[0]) << 8) |
	(unsigned long) eth->mac_address[1];
    case ETH_HASH0:
      return eth->regs.hash0;
    case ETH_HASH1:
      return eth->regs.hash1;
      /*case ETH_DMA_RX_TX: return eth_rx( eth ); */
    }

  if ((addr >= ETH_BD_BASE) && (addr < ETH_BD_BASE + ETH_BD_SPACE))
    return eth->regs.bd_ram[(addr - ETH_BD_BASE) / 4];

  PRINTF ("eth_read32( 0x%" PRIxADDR " ): Illegal address\n",
	  addr + eth->baseaddr);
  return 0;
}

/* ========================================================================= */


/* 
  Write a register 
*/
static void
eth_write32 (oraddr_t addr, uint32_t value, void *dat)
{
  struct eth_device *eth = dat;
  unsigned char      buf[ETH_MAXPL];

  switch (addr)
    {
    case ETH_MODER:
#if ETH_DEBUG
      printf("eth_write32: MODER 0x%x\n",value);
#endif
      if (!TEST_FLAG (eth->regs.moder, ETH_MODER, RXEN) &&
	  TEST_FLAG (value, ETH_MODER, RXEN))
	{
	  // Reset RX BD index
	  eth->rx_bd_index = eth->regs.tx_bd_num << 1;
	  
	  // Clear TAP
	  {
	    /* Poll to see if there is data to read */
	    struct pollfd  fds[1];
	    int    n;
	    int nread;
	    
	    fds[0].fd = eth->rtx_fd;
	    fds[0].events = POLLIN;
	    
	    do {
	      n = poll (fds, 1, 0);
	      if (n < 0)
		{
		  fprintf (stderr, "Warning: Poll in while emptying TAP: %s: ignored.\n",
			   strerror (errno));
		}
	      else if ((n > 0) && ((fds[0].revents & POLLIN) == POLLIN))
		{
		  nread = read (eth->rtx_fd, buf, ETH_MAXPL);
		  
		  if (nread < 0)
		    {
		      fprintf (stderr,
			       "Warning: Read failed %s: ignored\n",
			       strerror (errno));
		    }	    
		}
	    } while (n > 0);
	  }
	  
	  SCHED_ADD (eth_controller_rx_clock, dat, 1);
	}
      else if (!TEST_FLAG (value, ETH_MODER, RXEN) &&
	       TEST_FLAG (eth->regs.moder, ETH_MODER, RXEN))
	SCHED_FIND_REMOVE (eth_controller_rx_clock, dat);

      if (!TEST_FLAG (eth->regs.moder, ETH_MODER, TXEN) &&
	  TEST_FLAG (value, ETH_MODER, TXEN))
	{
	  eth->tx_bd_index = 0;
	  SCHED_ADD (eth_controller_tx_clock, dat, 1);
	}
      else if (!TEST_FLAG (value, ETH_MODER, TXEN) &&
	       TEST_FLAG (eth->regs.moder, ETH_MODER, TXEN))
	SCHED_FIND_REMOVE (eth_controller_tx_clock, dat);

      eth->regs.moder = value;

      if (TEST_FLAG (value, ETH_MODER, RST))
	eth_reset (dat);
      return;
    case ETH_INT_SOURCE:
#if ETH_DEBUG
      printf("eth_write32: INT_SOURCE 0x%x\n",value);
#endif
      eth->regs.int_source &= ~value;
      
      // Clear IRQ line if all interrupt sources have been dealt with
      if (!(eth->regs.int_source & eth->regs.int_mask) && eth->int_line_stat)
	{
	  clear_interrupt (eth->mac_int);
	  eth->int_line_stat = 0;
	}
      
      return;
    case ETH_INT_MASK:
#if ETH_DEBUG
      printf("eth_write32: INT_MASK 0x%x\n",value);
#endif
      eth->regs.int_mask = value;
      if ((eth->regs.int_source & eth->regs.int_mask) && !eth->int_line_stat)
	report_interrupt (eth->mac_int);
      else
	if (eth->int_line_stat)
	  {
	    clear_interrupt (eth->mac_int);
	    eth->int_line_stat = 0;
	  }
      return;
    case ETH_IPGT:
      eth->regs.ipgt = value;
      return;
    case ETH_IPGR1:
      eth->regs.ipgr1 = value;
      return;
    case ETH_IPGR2:
      eth->regs.ipgr2 = value;
      return;
    case ETH_PACKETLEN:
      eth->regs.packetlen = value;
      return;
    case ETH_COLLCONF:
      eth->regs.collconf = value;
      return;
    case ETH_TX_BD_NUM:
      /* When TX_BD_NUM is written, also reset current RX BD index */
      eth->regs.tx_bd_num = value & 0xFF;
      eth->rx_bd_index = eth->regs.tx_bd_num << 1;
      return;
    case ETH_CTRLMODER:
      eth->regs.controlmoder = value;
      return;
    case ETH_MIIMODER:
      eth->regs.miimoder = value;
      return;
    case ETH_MIICOMMAND:
      eth->regs.miicommand = value;
      /* Perform MIIM transaction, if required */
      eth_miim_trans (eth);
      return;
    case ETH_MIIADDRESS:
      eth->regs.miiaddress = value;
      return;
    case ETH_MIITX_DATA:
      eth->regs.miitx_data = value;
      return;
    case ETH_MIIRX_DATA:
      /* Register is R/O
      eth->regs.miirx_data = value;
      */
      return;
    case ETH_MIISTATUS:
      /* Register is R/O
      eth->regs.miistatus = value;
      */
      return;

    case ETH_MAC_ADDR0:
      eth->mac_address[5] = value & 0xFF;
      eth->mac_address[4] = (value >> 8) & 0xFF;
      eth->mac_address[3] = (value >> 16) & 0xFF;
      eth->mac_address[2] = (value >> 24) & 0xFF;
      return;
    case ETH_MAC_ADDR1:
      eth->mac_address[1] = value & 0xFF;
      eth->mac_address[0] = (value >> 8) & 0xFF;
      return;
    case ETH_HASH0:
      eth->regs.hash0 = value;
      return;
    case ETH_HASH1:
      eth->regs.hash1 = value;
      return;

      /*case ETH_DMA_RX_TX: eth_tx( eth, value ); return; */
    }

  if ((addr >= ETH_BD_BASE) && (addr < ETH_BD_BASE + ETH_BD_SPACE))
    {
      eth->regs.bd_ram[(addr - ETH_BD_BASE) / 4] = value;
      return;
    }

  PRINTF ("eth_write32( 0x%" PRIxADDR " ): Illegal address\n",
	  addr + eth->baseaddr);
  return;
}

/* ========================================================================= */


/* ========================================================================= */



/* ========================================================================= */

/*-----------------------------------------------[ Ethernet configuration ]---*/


/*---------------------------------------------------------------------------*/
/*!Enable or disable the Ethernet interface

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_enabled (union param_val  val,
	     void            *dat)
{
  struct eth_device *eth = dat;

  eth->enabled = val.int_val;

}	/* eth_enabled() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet interface base address

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_baseaddr (union param_val  val,
	      void            *dat)
{
  struct eth_device *eth = dat;

  eth->baseaddr = val.addr_val;

}	/* eth_baseaddr() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet DMA port

   This is not currently supported, so a warning message is printed.

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_dma (union param_val  val,
	 void            *dat)
{
  struct eth_device *eth = dat;

  fprintf (stderr, "Warning: External Ethernet DMA not currently supported\n");
  eth->dma = val.addr_val;

}	/* eth_dma() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet IRQ

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_irq (union param_val  val,
	 void            *dat)
{
  struct eth_device *eth = dat;

  eth->mac_int = val.int_val;

}	/* eth_irq() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet interface type

   Currently two types are supported, file and tap.

   @param[in] val  The value to use. Currently "file" and "tap" are supported.
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_rtx_type (union param_val  val,
	      void            *dat)
{
  struct eth_device *eth = dat;

  if (0 == strcasecmp ("file", val.str_val))
    {
      printf ("Ethernet FILE type\n");
      eth->rtx_type = ETH_RTX_FILE;
    }
  else if (0 == strcasecmp ("tap", val.str_val))
    {
      printf ("Ethernet TAP type\n");
      eth->rtx_type = ETH_RTX_TAP;
    }
  else
    {
      fprintf (stderr, "Warning: Unknown Ethernet type: file assumed.\n");
      eth->rtx_type = ETH_RTX_FILE;
    }
}	/* eth_rtx_type() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet DMA Rx channel

   External DMA is not currently supported, so a warning message is printed.

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_rx_channel (union param_val  val,
		void            *dat)
{
  struct eth_device *eth = dat;

  fprintf (stderr, "Warning: External Ethernet DMA not currently supported: "
	   "Rx channel ignored\n");
  eth->rx_channel = val.int_val;

}	/* eth_rx_channel() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet DMA Tx channel

   External DMA is not currently supported, so a warning message is printed.

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_tx_channel (union param_val  val,
		void            *dat)
{
  struct eth_device *eth = dat;

  fprintf (stderr, "Warning: External Ethernet DMA not currently supported: "
	   "Tx channel ignored\n");
  eth->tx_channel = val.int_val;

}	/* eth_tx_channel() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet DMA Rx file

   Free any previously allocated value.

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_rxfile (union param_val  val,
	    void            *dat)
{
  struct eth_device *eth = dat;

  if (NULL != eth->rxfile)
    {
      free (eth->rxfile);
      eth->rxfile = NULL;
    }

  if (!(eth->rxfile = strdup (val.str_val)))
    {
      fprintf (stderr, "Peripheral Ethernet: Run out of memory\n");
      exit (-1);
    }
}	/* eth_rxfile() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet DMA Tx file

   Free any previously allocated value.

   @param[in] val  The value to use
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_txfile (union param_val  val,
	    void            *dat)
{
  struct eth_device *eth = dat;

  if (NULL != eth->txfile)
    {
      free (eth->txfile);
      eth->txfile = NULL;
    }

  if (!(eth->txfile = strdup (val.str_val)))
    {
      fprintf (stderr, "Peripheral Ethernet: Run out of memory\n");
      exit (-1);
    }
}	/* eth_txfile() */


/*---------------------------------------------------------------------------*/
/*!Set the Ethernet TAP device.

   If we are not superuser (or do not have CAP_NET_ADMIN priviledges), then we
   must work with a persistent TAP device that is already set up. This option
   specifies the device to user.

   @param[in] val  The value to use.
   @param[in] dat  The config data structure                                 */
/*---------------------------------------------------------------------------*/
static void
eth_tap_dev (union param_val  val,
	      void            *dat)
{
  struct eth_device *eth = dat;

  if (NULL != eth->tap_dev)
    {
      free (eth->tap_dev);
      eth->tap_dev = NULL;
    }

  eth->tap_dev = strdup (val.str_val);

  if (NULL == eth->tap_dev)
    {
      fprintf (stderr, "ERROR: Peripheral Ethernet: Run out of memory\n");
      exit (-1);
    }
}	/* eth_tap_dev() */


static void
eth_vapi_id (union param_val  val,
	     void            *dat)
{
  struct eth_device *eth = dat;
  eth->base_vapi_id = val.int_val;
}


static void
eth_phy_addr (union param_val  val,
	      void            *dat)
{
  struct eth_device *eth = dat;
  eth->phy_addr = val.int_val & ETH_MIIADDR_FIAD_MASK;
}


/*---------------------------------------------------------------------------*/
/*!Initialize a new Ethernet configuration

   ALL parameters are set explicitly to default values.                      */
/*---------------------------------------------------------------------------*/
static void *
eth_sec_start (void)
{
  struct eth_device *new = malloc (sizeof (struct eth_device));

  if (!new)
    {
      fprintf (stderr, "Peripheral Eth: Run out of memory\n");
      exit (-1);
    }

  memset (new, 0, sizeof (struct eth_device));

  new->enabled      = 1;
  new->baseaddr     = 0;
  new->dma          = 0;
  new->mac_int      = 0;
  new->int_line_stat= 0;
  new->rtx_type     = ETH_RTX_FILE;
  new->rx_channel   = 0;
  new->tx_channel   = 0;
  new->rtx_fd       = 0;
  new->rxfile       = strdup ("eth_rx");
  new->txfile       = strdup ("eth_tx");
  new->tap_dev      = strdup ("");
  new->base_vapi_id = 0;
  new->phy_addr     = 0;

  return new;
}

static void
eth_sec_end (void *dat)
{
  struct eth_device *eth = dat;
  struct mem_ops ops;

  if (!eth->enabled)
    {
      free (eth->rxfile);
      free (eth->txfile);
      free (eth->tap_dev);
      free (eth);
      return;
    }

  memset (&ops, 0, sizeof (struct mem_ops));

  ops.readfunc32 = eth_read32;
  ops.writefunc32 = eth_write32;
  ops.read_dat32 = dat;
  ops.write_dat32 = dat;

  /* FIXME: Correct delay? */
  ops.delayr = 2;
  ops.delayw = 2;
  reg_mem_area (eth->baseaddr, ETH_ADDR_SPACE, 0, &ops);
  reg_sim_stat (eth_status, dat);
  reg_sim_reset (eth_reset, dat);
}


/*---------------------------------------------------------------------------*/
/*!Register a new Ethernet configuration                                     */
/*---------------------------------------------------------------------------*/
void
reg_ethernet_sec ()
{
  struct config_section *sec =
    reg_config_sec ("ethernet", eth_sec_start, eth_sec_end);

  reg_config_param (sec, "enabled",    PARAMT_INT,  eth_enabled);
  reg_config_param (sec, "baseaddr",   PARAMT_ADDR, eth_baseaddr);
  reg_config_param (sec, "dma",        PARAMT_INT,  eth_dma);
  reg_config_param (sec, "irq",        PARAMT_INT,  eth_irq);
  reg_config_param (sec, "rtx_type",   PARAMT_STR,  eth_rtx_type);
  reg_config_param (sec, "rx_channel", PARAMT_INT,  eth_rx_channel);
  reg_config_param (sec, "tx_channel", PARAMT_INT,  eth_tx_channel);
  reg_config_param (sec, "rxfile",     PARAMT_STR,  eth_rxfile);
  reg_config_param (sec, "txfile",     PARAMT_STR,  eth_txfile);
  reg_config_param (sec, "tap_dev",    PARAMT_STR,  eth_tap_dev);
  reg_config_param (sec, "vapi_id",    PARAMT_INT,  eth_vapi_id);
  reg_config_param (sec, "phy_addr",   PARAMT_INT,  eth_phy_addr);

}	/* reg_ethernet_sec() */

