/*
 *      Copyright (C) 2011 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 *  Initial design and concept by Gabest and the MPC-HC Team, copyright under GPLv2
 */

#include "stdafx.h"
#include "StreamParser.h"

#include "OutputPin.h"
#include "H264Nalu.h"

extern "C" {
#include "libavutil/intreadwrite.h"
}

//#define DEBUG_PGS_PARSER

CStreamParser::CStreamParser(CLAVOutputPin *pPin, const char *szContainer)
  : m_pPin(pPin), m_strContainer(szContainer), m_pPacketBuffer(NULL), m_gSubtype(GUID_NULL), m_bPGSDropState(FALSE)
{

}


CStreamParser::~CStreamParser()
{
  Flush();
}

HRESULT CStreamParser::Parse(const GUID &gSubtype, Packet *pPacket)
{
  if (gSubtype != m_gSubtype) {
    m_gSubtype = gSubtype;
    Flush();
  }
  
  if (!pPacket || (pPacket->dwFlags & LAV_PACKET_PARSED)) {
    Queue(pPacket);
  } else if ((m_strContainer == "mpegts" || m_strContainer == "mpeg" || m_strContainer == "avi") && m_gSubtype == MEDIASUBTYPE_AVC1) {
    ParseH264AnnexB(pPacket);
  } else if ((m_strContainer == "mpegts" || m_strContainer == "mpeg") && (m_gSubtype == MEDIASUBTYPE_WVC1 || m_gSubtype == MEDIASUBTYPE_WVC1_ARCSOFT || m_gSubtype == MEDIASUBTYPE_WVC1_CYBERLINK)) {
    ParseVC1(pPacket);
  } else if (m_gSubtype == MEDIASUBTYPE_HDMVSUB) {
    ParsePGS(pPacket);
  } else if (m_gSubtype == MEDIASUBTYPE_HDMV_LPCM_AUDIO) {
    pPacket->RemoveHead(4);
    Queue(pPacket);
  } else if (pPacket->dwFlags & LAV_PACKET_MOV_TEXT) {
    ParseMOVText(pPacket);
  } else {
    Queue(pPacket);
  }

  return S_OK;
}

HRESULT CStreamParser::Flush()
{
  DbgLog((LOG_TRACE, 10, L"CStreamParser::Flush()"));
  SAFE_DELETE(m_pPacketBuffer);
  m_queue.Clear();
  m_bPGSDropState = FALSE;
  m_bHasAccessUnitDelimiters = false;

  return S_OK;
}

HRESULT CStreamParser::Queue(Packet *pPacket) const
{
  return m_pPin->QueueFromParser(pPacket);
}

static Packet *InitPacket(Packet *pSource)
{
  Packet *pNew = NULL;

  pNew = new Packet();
  pNew->StreamId = pSource->StreamId;
  pNew->bDiscontinuity = pSource->bDiscontinuity;
  pSource->bDiscontinuity = FALSE;

  pNew->bSyncPoint = pSource->bSyncPoint;
  pSource->bSyncPoint = FALSE;

  pNew->rtStart = pSource->rtStart;
  pSource->rtStart = Packet::INVALID_TIME;

  pNew->rtStop = pSource->rtStop;
  pSource->rtStop = Packet::INVALID_TIME;

  pNew->pmt = pSource->pmt;
  pSource->pmt = NULL;

  return pNew;
}

#define MOVE_TO_H264_START_CODE(b, e) while(b <= e-4 && !((*(DWORD *)b == 0x01000000) || ((*(DWORD *)b & 0x00FFFFFF) == 0x00010000))) b++; if((b <= e-4) && *(DWORD *)b == 0x01000000) b++;

HRESULT CStreamParser::ParseH264AnnexB(Packet *pPacket)
{
  if (!m_pPacketBuffer) {
    m_pPacketBuffer = InitPacket(pPacket);
  }

  m_pPacketBuffer->Append(pPacket);

  BYTE *start = m_pPacketBuffer->GetData();
  BYTE *end = start + m_pPacketBuffer->GetDataSize();

  MOVE_TO_H264_START_CODE(start, end);

  while(start <= end-4) {
    BYTE *next = start + 1;

    MOVE_TO_H264_START_CODE(next, end);

    // End of buffer reached
    if(next >= end-4) {
      break;
    }

    size_t size = next - start;

    CH264Nalu Nalu;
    Nalu.SetBuffer(start, (int)size, 0);

    Packet *p2 = NULL;

    while (Nalu.ReadNext()) {
      Packet *p3 = new Packet();
      p3->SetDataSize(Nalu.GetDataLength() + 4);

      // Write size of the NALU (Big Endian)
      AV_WB32(p3->GetData(), Nalu.GetDataLength());
      memcpy(p3->GetData() + 4, Nalu.GetDataBuffer(), Nalu.GetDataLength());

      if (!p2) {
        p2 = p3;
      } else {
        p2->Append(p3);
        SAFE_DELETE(p3);
      }
    }

    if (!p2)
      break;

    p2->StreamId = m_pPacketBuffer->StreamId;
    p2->bDiscontinuity = m_pPacketBuffer->bDiscontinuity;
    m_pPacketBuffer->bDiscontinuity = FALSE;

    p2->bSyncPoint = m_pPacketBuffer->bSyncPoint;
    m_pPacketBuffer->bSyncPoint = FALSE;

    p2->rtStart = m_pPacketBuffer->rtStart;
    m_pPacketBuffer->rtStart = Packet::INVALID_TIME;
    p2->rtStop = m_pPacketBuffer->rtStop;
    m_pPacketBuffer->rtStop = Packet::INVALID_TIME;

    p2->pmt = m_pPacketBuffer->pmt;
    m_pPacketBuffer->pmt = NULL;

    m_queue.Queue(p2, FALSE);

    if(pPacket->rtStart != Packet::INVALID_TIME) {
      m_pPacketBuffer->rtStart = pPacket->rtStart;
      m_pPacketBuffer->rtStop = pPacket->rtStop;
      pPacket->rtStart = Packet::INVALID_TIME;
    }
    if(pPacket->bDiscontinuity) {
      m_pPacketBuffer->bDiscontinuity = pPacket->bDiscontinuity;
      pPacket->bDiscontinuity = FALSE;
    }
    if(pPacket->bSyncPoint) {
      m_pPacketBuffer->bSyncPoint = pPacket->bSyncPoint;
      pPacket->bSyncPoint = FALSE;
    }
    if(m_pPacketBuffer->pmt) {
      DeleteMediaType(m_pPacketBuffer->pmt);
    }

    m_pPacketBuffer->pmt = pPacket->pmt;
    pPacket->pmt = NULL;

    start = next;
  }

  if(start > m_pPacketBuffer->GetData()) {
    m_pPacketBuffer->RemoveHead(start - m_pPacketBuffer->GetData());
  }

  SAFE_DELETE(pPacket);

  do {
    pPacket = NULL;

    REFERENCE_TIME rtStart = Packet::INVALID_TIME, rtStop = rtStart = Packet::INVALID_TIME;

    std::deque<Packet *>::iterator it;
    for (it = m_queue.GetQueue()->begin(); it != m_queue.GetQueue()->end(); ++it) {
      // Skip the first
      if (it == m_queue.GetQueue()->begin()) {
        continue;
      }

      Packet *p = *it;
      BYTE* pData = p->GetData();

      if((pData[4]&0x1f) == 0x09) {
        m_bHasAccessUnitDelimiters = true;
      }

      if ((pData[4]&0x1f) == 0x09 || (!m_bHasAccessUnitDelimiters && p->rtStart != Packet::INVALID_TIME)) {
        pPacket = p;
        if (p->rtStart == Packet::INVALID_TIME && rtStart != Packet::INVALID_TIME) {
          p->rtStart = rtStart;
          p->rtStop = rtStop;
        }
        break;
      }

      if (rtStart == Packet::INVALID_TIME) {
        rtStart = p->rtStart;
        rtStop = p->rtStop;
      }
    }

    if (pPacket) {
      Packet *p = m_queue.Get();
      Packet *p2 = NULL;
      while ((p2 = m_queue.Get()) != pPacket) {
        p->Append(p2);
        SAFE_DELETE(p2);
      }
      // Return
      m_queue.GetQueue()->push_front(pPacket);

      Queue(p);
    }
  } while (pPacket != NULL);

  return S_OK;
}

HRESULT CStreamParser::ParseVC1(Packet *pPacket)
{
  if (!m_pPacketBuffer) {
    m_pPacketBuffer = InitPacket(pPacket);
  }

  m_pPacketBuffer->Append(pPacket);

  BYTE *start = m_pPacketBuffer->GetData();
  BYTE *end = start + m_pPacketBuffer->GetDataSize();

  bool bSeqFound = false;
  while(start <= end-4) {
    if (*(DWORD *)start == 0x0D010000) {
      bSeqFound = true;
      break;
    } else if (*(DWORD *)start == 0x0F010000) {
      break;
    }
    start++;
  }

  while(start <= end-4) {
    BYTE *next = start+1;

    while(next <= end-4) {
      if (*(DWORD *)next == 0x0D010000) {
        if (bSeqFound) {
          break;
        }
        bSeqFound = true;
      } else if (*(DWORD*)next == 0x0F010000) {
        break;
      }
      next++;
    }

    if(next >= end-4) {
      break;
    }

    Packet *p2 = new Packet();
    p2->StreamId = m_pPacketBuffer->StreamId;
    p2->bDiscontinuity = m_pPacketBuffer->bDiscontinuity;
    m_pPacketBuffer->bDiscontinuity = FALSE;

    p2->bSyncPoint = m_pPacketBuffer->bSyncPoint;
    m_pPacketBuffer->bSyncPoint = FALSE;

    p2->rtStart = m_pPacketBuffer->rtStart;
    m_pPacketBuffer->rtStart = Packet::INVALID_TIME;

    p2->rtStop = m_pPacketBuffer->rtStop;
    m_pPacketBuffer->rtStop = Packet::INVALID_TIME;

    p2->pmt = m_pPacketBuffer->pmt;
    m_pPacketBuffer->pmt = NULL;

    p2->SetData(start, next - start);

    Queue(p2);

    if (pPacket->rtStart != Packet::INVALID_TIME) {
      m_pPacketBuffer->rtStart = pPacket->rtStart;
      m_pPacketBuffer->rtStop = pPacket->rtStart;
      pPacket->rtStart = Packet::INVALID_TIME;
    }

    if (pPacket->bDiscontinuity) {
      m_pPacketBuffer->bDiscontinuity = pPacket->bDiscontinuity;
      pPacket->bDiscontinuity = FALSE;
    }

    if (pPacket->bSyncPoint) {
      m_pPacketBuffer->bSyncPoint = pPacket->bSyncPoint;
      pPacket->bSyncPoint = FALSE;
    }

    m_pPacketBuffer->pmt = pPacket->pmt;
    pPacket->pmt = NULL;

    start = next;
    bSeqFound = (*(DWORD*)start == 0x0D010000);
  }

  if(start > m_pPacketBuffer->GetData()) {
    m_pPacketBuffer->RemoveHead(start - m_pPacketBuffer->GetData());
  }

  SAFE_DELETE(pPacket);

  return S_OK;
}

HRESULT CStreamParser::ParsePGS(Packet *pPacket)
{
  const uint8_t *buf = pPacket->GetData();
  const size_t buf_size = pPacket->GetDataSize();

  const uint8_t *buf_end = buf + buf_size;
  uint8_t       segment_type;
  int           segment_length;

  if (buf_size < 3) {
    DbgLog((LOG_TRACE, 30, L"::ParsePGS(): Way too short PGS packet"));
    goto done;
  }

  m_pgsBuffer.SetSize(0);

  while(buf < buf_end) {
    const uint8_t *segment_start = buf;
    segment_type   = AV_RB8(buf);
    segment_length = AV_RB16(buf+1);

    buf += 3;

#ifdef DEBUG_PGS_PARSER
    DbgLog((LOG_TRACE, 50, L"::ParsePGS(): segment_type: 0x%x, segment_length: %d", segment_type, segment_length));
#endif

    // Presentation segment
    if (segment_type == 0x16) {
      // Segment Layout
      // 2 bytes width
      // 2 bytes height
      // 1 unknown byte
      // 2 bytes id
      // 1 byte composition state (0x00 = normal, 0x40 = ACQU_POINT (?), 0x80 = epoch start (new frame), 0xC0 = epoch continue)
      // 2 unknown bytes
      // 1 byte object number
      uint8_t objectNumber = buf[10];
      // 2 bytes object ref id
      if (objectNumber == 0) {
        m_bPGSDropState = FALSE;
      } else if (segment_length >= 0x13) {
        // 1 byte window_id
        // 1 byte object_cropped_flag: 0x80, forced_on_flag = 0x040, 6bit reserved
        uint8_t forced_flag = buf[14];
        m_bPGSDropState = !(forced_flag & 0x40);
        // 2 bytes x
        // 2 bytes y
        // total length = 19 bytes
      }
#ifdef DEBUG_PGS_PARSER
      DbgLog((LOG_TRACE, 50, L"::ParsePGS(): Presentation Segment! obj.num: %d; state: 0x%x; dropping: %d", objectNumber, buf[7], m_bPGSDropState));
#endif
    }
    if (!m_bPGSDropState) {
      m_pgsBuffer.Append(segment_start, segment_length + 3);
    }

    buf += segment_length;
  }

  if (m_pgsBuffer.GetCount() > 0) {
    pPacket->SetData(m_pgsBuffer.Ptr(), m_pgsBuffer.GetCount());
  } else {
    delete pPacket;
    return S_OK;
  }

done:
  return Queue(pPacket);
}

HRESULT CStreamParser::ParseMOVText(Packet *pPacket)
{
  size_t avail = pPacket->GetDataSize();
  BYTE *ptr = pPacket->GetData();
  if (avail > 2) {  
    unsigned size = (ptr[0] << 8) | ptr[1];
    if (size <= avail-2) {
      pPacket->RemoveHead(2);
      pPacket->SetDataSize(size);
      return Queue(pPacket);
    }
  }
  SAFE_DELETE(pPacket);
  return S_FALSE;
}