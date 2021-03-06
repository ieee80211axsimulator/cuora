/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2006, 2009 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *          Mirko Banchi <mk.banchi@gmail.com>
 */

#include "sta-wifi-mac.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "mac-low.h"

/*
 * The state machine for this STA is:
 --------------                                          -----------
 | Associated |   <--------------------      ------->    | Refused |
 --------------                        \    /            -----------
    \                                   \  /
     \    -----------------     -----------------------------
      \-> | Beacon Missed | --> | Wait Association Response |
          -----------------     -----------------------------
                \                       ^
                 \                      |
                  \    -----------------------
                   \-> | Wait Probe Response |
                       -----------------------
 */

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("StaWifiMac");

NS_OBJECT_ENSURE_REGISTERED (StaWifiMac);

bool StaWifiMac::m_bsrTx0 = false;
bool StaWifiMac::m_bsrTx1 = false;
bool StaWifiMac::m_bsrTx2 = false;
bool StaWifiMac::m_bsrTx3 = false;
bool StaWifiMac::m_bsrTx4 = false;
bool StaWifiMac::m_bsrTx5 = false;
bool StaWifiMac::m_bsrTx6 = false;
bool StaWifiMac::m_bsrTx7 = false;
bool StaWifiMac::m_bsrTx8 = false;

TypeId
StaWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::StaWifiMac")
    .SetParent<RegularWifiMac> ()
    .SetGroupName ("Wifi")
    .AddConstructor<StaWifiMac> ()
    .AddAttribute ("ProbeRequestTimeout", "The interval between two consecutive probe request attempts.",
                   TimeValue (Seconds (0.05)),
                   MakeTimeAccessor (&StaWifiMac::m_probeRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("AssocRequestTimeout", "The interval between two consecutive assoc request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&StaWifiMac::m_assocRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("MaxMissedBeacons",
                   "Number of beacons which much be consecutively missed before "
                   "we attempt to restart association.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&StaWifiMac::m_maxMissedBeacons),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ActiveProbing",
                   "If true, we send probe requests. If false, we don't."
                   "NOTE: if more than one STA in your simulation is using active probing, "
                   "you should enable it at a different simulation time for each STA, "
                   "otherwise all the STAs will start sending probes at the same time resulting in collisions. "
                   "See bug 1060 for more info.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&StaWifiMac::SetActiveProbing, &StaWifiMac::GetActiveProbing),
                   MakeBooleanChecker ())
    .AddTraceSource ("Assoc", "Associated with an access point.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_assocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("DeAssoc", "Association with an access point lost.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_deAssocLogger),
                     "ns3::Mac48Address::TracedCallback")
  ;
  return tid;
}

StaWifiMac::StaWifiMac ()
  : m_state (BEACON_MISSED),
    m_probeRequestEvent (),
    m_assocRequestEvent (),
    m_beaconWatchdogEnd (Seconds (0))
{
  NS_LOG_FUNCTION (this);

  //Let the lower layers know that we are acting as a non-AP STA in
  //an infrastructure BSS.
  SetMuMode (false);
  m_muUlModeEnd = Seconds(0);
  m_muDlModeEnd = Seconds(0);
  m_noSlots = 0;
  m_updatedOnce = false;
  m_firstTf = true;
  m_bsrAckRecvd = true;
  SetTypeOfStation (STA);
  for (uint32_t ru = 0; ru < 9; ru++)
   {
     m_lowMu[ru]->SetTfRespAccessGrantCallback (MakeCallback (&StaWifiMac::TriggerFrameRespAccess, this));
   }
  RegularWifiMac::RegisterTfListener (this);
}

StaWifiMac::~StaWifiMac ()
{
  NS_LOG_FUNCTION (this);
}

void
StaWifiMac::TriggerFrameRespAccess (void)
{
  Simulator::Schedule (MicroSeconds (1), &DcfManager::NotifyMaybeCcaBusyStartNow, m_dcfManagerMu[GetRuBits ()], Seconds(1)); //Hack
  /*
   * Crude way of notifying other STAs that I have gotten access
   * to the channel on this RU for this TF cycle.
   * So if you have a BSR scheduled in this TF cycle,
   * then you must cancel it.
   */
  //std::cout<<"In TriggerFrameRespAccess of node "<<m_phy->GetDevice ()->GetNode()->GetId () <<"\tru = "<<GetRuBits () <<"\ttime = "<<Now ().GetMicroSeconds ()<<std::endl;
  RegularWifiMac::NotifyTfRespAccess (GetRuBits ());
}

void
StaWifiMac::UpdateSlots (uint32_t ru)
{
  int nus;
  uint32_t nIntSlots;
  //std::cout<<"In UpdateSlots of node "<<m_phy->GetDevice ()->GetNode()->GetId () <<"\tru = "<<ru <<"\t myRU = "<<GetRuBits () <<"\ttime = "<<Now ().GetMicroSeconds ()<<std::endl;
  if (ru == GetRuBits () && m_noSlots != 0 && !m_updatedOnce)
   {
     m_cancelEvent.Cancel ();
     nus = (Now () - m_lastTfRespRecv).GetMicroSeconds ();
     if (nus % GetSlot ().GetMicroSeconds () != 0)
      {
        nus -= GetSifs ().GetMicroSeconds ();
      }
     nIntSlots = nus / GetSlot ().GetMicroSeconds ();
     m_noSlots -= (nIntSlots);
     /* If multiple STAs transmit at the same time,
      * this function will be called multiple times
      * Need to ensure that I do not update my OBO
      * for each such instance, and must update only 
      * once.
      */ 
     m_updatedOnce = true;
     //std::cout<<"In StaWifiMac::UpdateSlots of node "<<m_phy->GetDevice ()->GetNode ()->GetId ()<<", m_lastTfRespRecv = "<<m_lastTfRespRecv.GetMicroSeconds ()<<", nus = "<<nus<<", nIntSlots = "<<nIntSlots<<", ru = "<<ru<<", m_noSlots = "<<m_noSlots<<"\ttime = "<<Now ().GetMicroSeconds ()<<std::endl;
   }
}

void 
StaWifiMac::UpdateBsrTx (uint32_t ru)
{
  if (ru == 0)
   m_bsrTx0 = true;
  else if (ru == 1)
   m_bsrTx1 = true;
  else if (ru == 2)
   m_bsrTx2 = true;
  else if (ru == 3)
   m_bsrTx3 = true;
  else if (ru == 4)
   m_bsrTx4 = true;
  else if (ru == 5)
   m_bsrTx5 = true;
  else if (ru == 6)
   m_bsrTx6 = true;
  else if (ru == 7)
   m_bsrTx7 = true;
  else if (ru == 8)
   m_bsrTx8 = true;
}

void 
StaWifiMac::ResetBsrTx ()
{
   m_bsrTx0 = false;
   m_bsrTx1 = false;
   m_bsrTx2 = false;
   m_bsrTx3 = false;
   m_bsrTx4 = false;
   m_bsrTx5 = false;
   m_bsrTx6 = false;
   m_bsrTx7 = false;
   m_bsrTx8 = false;
}

bool
StaWifiMac::GetBsrTx (uint32_t ru)
{
  if (ru == 0)
    return m_bsrTx0;
  else if (ru == 1)
    return m_bsrTx1;
  else if (ru == 2)
    return m_bsrTx2;
  else if (ru == 3)
    return m_bsrTx3;
  else if (ru == 4)
    return m_bsrTx4;
  else if (ru == 5)
    return m_bsrTx5;
  else if (ru == 6)
    return m_bsrTx6;
  else if (ru == 7)
    return m_bsrTx7;
  else if (ru == 8)
    return m_bsrTx8;

    return false;
}

void
StaWifiMac::CheckAndCancel (uint32_t ru)
{
  if (GetBsrTx (ru))
   {
     //std::cout<<"In CheckAndCancel of node "<<m_phy->GetDevice()->GetNode ()->GetId ()<<", GetBsrTx("<<ru<<") = true, canceling BSR transmission, time = "<<Now ().GetMicroSeconds ()<<std::endl;
     /*
      * This function is called just before the TF response is scheduled
      * If any BSR is already sent on this RU before I am scheduled to transmit
      * then I cancel my BSR transmission
      */
     m_triggerFrameRespEvent.Cancel ();
   }
  else
   {
     //std::cout<<"In CheckAndCancel of node "<<m_phy->GetDevice()->GetNode ()->GetId ()<<", GetBsrTx("<<ru<<") = false, transmitting BSR, time = "<<Now ().GetMicroSeconds ()<<std::endl;
     /* 
      * If no one else has already transmitted on this RU, then I must 
      * transmit my BSR and update the static variables that will be
      * used by other STAs. Also, on this RU, after my BSR transmission
      * is completed, I must not send a payload transmission.
      */
     Simulator::Schedule(MicroSeconds (1), &StaWifiMac::UpdateBsrTx, ru);
   }
}
 
void
StaWifiMac::CancelExpiredEvents (void)
{
  //std::cout<<"In CancelExpiredEvents of node "<<m_phy->GetDevice()->GetNode ()->GetId ()<<", time = "<<Now ().GetMicroSeconds ()<<std::endl;
  m_edcaMu[GetRuBits ()][AC_BE]->CancelTFRespIfNotSent ();
  if (!GetBsrTx (GetRuBits ())) // This condition will occur only if all STAs contending on an RU have BO > MaxTFSlots; in this case decrement BO by MaxTfSlots.
   {	
     std::cout<<"TF cycle wasted because no STA sent BSR\n";
     m_noSlots -= (GetMaxTfSlots () - 1);
   }
}

void
StaWifiMac::SetMaxMissedBeacons (uint32_t missed)
{
  NS_LOG_FUNCTION (this << missed);
  m_maxMissedBeacons = missed;
}

void
StaWifiMac::SetProbeRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_probeRequestTimeout = timeout;
}

void
StaWifiMac::SetAssocRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_assocRequestTimeout = timeout;
}

void
StaWifiMac::StartActiveAssociation (void)
{
  NS_LOG_FUNCTION (this);
  TryToEnsureAssociated ();
}

void
StaWifiMac::SetActiveProbing (bool enable)
{
  NS_LOG_FUNCTION (this << enable);
  if (enable)
    {
      Simulator::ScheduleNow (&StaWifiMac::TryToEnsureAssociated, this);
    }
  else
    {
      m_probeRequestEvent.Cancel ();
    }
  m_activeProbing = enable;
}

bool StaWifiMac::GetActiveProbing (void) const
{
  return m_activeProbing;
}

void
StaWifiMac::SendProbeRequest (void)
{
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetProbeReq ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeRequestHeader probe;
  probe.SetSsid (GetSsid ());
  probe.SetSupportedRates (GetSupportedRates ());
  if (m_htSupported || m_vhtSupported || m_heSupported)
    {
      probe.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();
    }
  if (m_vhtSupported || m_heSupported)
    {
      probe.SetVhtCapabilities (GetVhtCapabilities ());
    }
  if (m_heSupported)
    {
      probe.SetHeCapabilities (GetHeCapabilities ());
    }
  packet->AddHeader (probe);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_dca->Queue (packet, hdr);

  if (m_probeRequestEvent.IsRunning ())
    {
      m_probeRequestEvent.Cancel ();
    }
  m_probeRequestEvent = Simulator::Schedule (m_probeRequestTimeout,
                                             &StaWifiMac::ProbeRequestTimeout, this);
}

uint32_t
StaWifiMac::GetBSR (void)
{
  return m_lowMu[GetRuBits ()]->CalculateStaPayloadDuration ();
}

void
StaWifiMac::SendTriggerFrameResp (uint32_t ru)
{
  //std::cout<<"In SendTriggerFrameResp of node "<<m_phy->GetDevice ()->GetNode ()->GetId () <<"\tru = "<<ru<<"\ttime = "<<Now ().GetMicroSeconds ()<< std::endl;
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetTriggerFrameResp ();
  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();

  MgtTFRespHeader resp;
  //Addsomething here
  resp.SetData (GetBSR ());
  resp.SetRu (ru);
  packet->AddHeader (resp);

  //NS_LOG_UNCOND("Inside node "<< m_phyMu[ru]->GetDevice()->GetNode ()->GetId ()<<" Sending TF response over RU = "<<m_phyMu[ru]->GetRuBits () << " time = "<<Simulator::Now ().GetMicroSeconds ());
  m_edcaMu[ru][AC_BE]->QueueTFResp (packet, hdr); // Push TF Response at the front of the queue
  m_dcfManagerMu[ru]->UpdateBusyDuration ();
  m_edcaMu[ru][AC_BE]->StartAccessIfNeeded ();
  m_dcfManagerMu[ru]->DoRestartAccessTimeoutIfNeeded ();
}

void
StaWifiMac::SendAssociationRequest (void)
{
  NS_LOG_FUNCTION (this << GetBssid ());
  WifiMacHeader hdr;
  hdr.SetAssocReq ();
  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetBssid ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtAssocRequestHeader assoc;
  assoc.SetSsid (GetSsid ());
  assoc.SetSupportedRates (GetSupportedRates ());
  assoc.SetCapabilities (GetCapabilities ());
  if (m_htSupported || m_vhtSupported || m_heSupported)
    {
      assoc.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();
    }
  if (m_vhtSupported || m_heSupported)
    {
      assoc.SetVhtCapabilities (GetVhtCapabilities ());
    }
  if (m_heSupported)
    {
      assoc.SetHeCapabilities (GetHeCapabilities ());
    }
  packet->AddHeader (assoc);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_dca->Queue (packet, hdr);

  if (m_assocRequestEvent.IsRunning ())
    {
      m_assocRequestEvent.Cancel ();
    }
  m_assocRequestEvent = Simulator::Schedule (m_assocRequestTimeout,
                                             &StaWifiMac::AssocRequestTimeout, this);
}

void
StaWifiMac::TryToEnsureAssociated (void)
{
  NS_LOG_FUNCTION (this);
  switch (m_state)
    {
    case ASSOCIATED:
      return;
      break;
    case WAIT_PROBE_RESP:
      /* we have sent a probe request earlier so we
         do not need to re-send a probe request immediately.
         We just need to wait until probe-request-timeout
         or until we get a probe response
       */
      break;
    case BEACON_MISSED:
      /* we were associated but we missed a bunch of beacons
       * so we should assume we are not associated anymore.
       * We try to initiate a probe request now.
       */
      m_linkDown ();
      if (m_activeProbing)
        {
          SetState (WAIT_PROBE_RESP);
          SendProbeRequest ();
        }
      break;
    case WAIT_ASSOC_RESP:
      /* we have sent an assoc request so we do not need to
         re-send an assoc request right now. We just need to
         wait until either assoc-request-timeout or until
         we get an assoc response.
       */
      break;
    case REFUSED:
      /* we have sent an assoc request and received a negative
         assoc resp. We wait until someone restarts an
         association with a given ssid.
       */
      break;
    }
}

void
StaWifiMac::AssocRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_ASSOC_RESP);
  SendAssociationRequest ();
}

void
StaWifiMac::ProbeRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_PROBE_RESP);
  SendProbeRequest ();
}

void
StaWifiMac::MissedBeacons (void)
{
  NS_LOG_FUNCTION (this);
  if (m_beaconWatchdogEnd > Simulator::Now ())
    {
      if (m_beaconWatchdog.IsRunning ())
        {
          m_beaconWatchdog.Cancel ();
        }
      m_beaconWatchdog = Simulator::Schedule (m_beaconWatchdogEnd - Simulator::Now (),
                                              &StaWifiMac::MissedBeacons, this);
      return;
    }
  NS_LOG_DEBUG ("beacon missed");
  SetState (BEACON_MISSED);
  TryToEnsureAssociated ();
}

void
StaWifiMac::RestartBeaconWatchdog (Time delay)
{
  NS_LOG_FUNCTION (this << delay);
  m_beaconWatchdogEnd = std::max (Simulator::Now () + delay, m_beaconWatchdogEnd);
  if (Simulator::GetDelayLeft (m_beaconWatchdog) < delay
      && m_beaconWatchdog.IsExpired ())
    {
      NS_LOG_DEBUG ("really restart watchdog.");
      m_beaconWatchdog = Simulator::Schedule (delay, &StaWifiMac::MissedBeacons, this);
    }
}

bool
StaWifiMac::IsAssociated (void) const
{
  return m_state == ASSOCIATED;
}

bool
StaWifiMac::IsWaitAssocResp (void) const
{
  return m_state == WAIT_ASSOC_RESP;
}

void
StaWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NS_LOG_FUNCTION (this << packet << to);
  if (!IsAssociated ())
    {
      NotifyTxDrop (packet);
      TryToEnsureAssociated ();
      return;
    }
  WifiMacHeader hdr;

  //If we are not a QoS AP then we definitely want to use AC_BE to
  //transmit the packet. A TID of zero will map to AC_BE (through \c
  //QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

  //For now, an AP that supports QoS does not support non-QoS
  //associations, and vice versa. In future the AP model should
  //support simultaneously associated QoS and non-QoS STAs, at which
  //point there will need to be per-association QoS state maintained
  //by the association state machine, and consulted here.
  if (m_qosSupported)
    {
      hdr.SetType (WIFI_MAC_QOSDATA);
      hdr.SetQosAckPolicy (WifiMacHeader::NORMAL_ACK);
      hdr.SetQosNoEosp ();
      hdr.SetQosNoAmsdu ();
      //Transmission of multiple frames in the same TXOP is not
      //supported for now
      hdr.SetQosTxopLimit (0);

      //Fill in the QoS control field in the MAC header
      tid = QosUtilsGetTidForPacket (packet);
      //Any value greater than 7 is invalid and likely indicates that
      //the packet had no QoS tag, so we revert to zero, which'll
      //mean that AC_BE is used.
      if (tid > 7)
        {
          tid = 0;
        }
      hdr.SetQosTid (tid);
    }
  else
    {
      hdr.SetTypeData ();
    }
  if (m_htSupported || m_vhtSupported || m_heSupported)
    {
      hdr.SetNoOrder ();
    }

  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (m_low->GetAddress ());
  hdr.SetAddr3 (to);
  hdr.SetDsNotFrom ();
  hdr.SetDsTo ();


  if (GetMuMode ())
   {
     if (m_qosSupported)
      {
        m_edcaMu[GetRuBits()][AC_BE]->Queue (packet, hdr);
      }
     else
      {
        m_dcaMu[GetRuBits()]->Queue (packet, hdr);
      }
   }
  else if (m_muModeToStart)
   {
      if (m_qosSupported)
       {
        m_edcaMu[GetRuBits()][AC_BE]->QueueButDontSend (packet, hdr);
       }
      else
       {
         m_dcaMu[GetRuBits()]->QueueButDontSend (packet, hdr);
       }
   }
  else 
   {
     for (uint32_t i = 0; i < 9; i++)
      {
        m_edcaMu[i][AC_BE]->QueueButDontSend (packet, hdr);
      }
     if (m_qosSupported)
      {
	m_dca->Queue (packet, hdr);
      }
     else
      {
        m_edca[QosUtilsMapTidToAc (tid)]->Queue (packet, hdr);
      }
   }
  
}

void
StaWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
  NS_LOG_FUNCTION (this << packet << hdr);
  NS_ASSERT (!hdr->IsCtl ());
  if (hdr->GetAddr3 () == GetAddress ())
    {
      NS_LOG_LOGIC ("packet sent by us.");
      return;
    }
  else if (hdr->GetAddr1 () != GetAddress ()
           && !hdr->GetAddr1 ().IsGroup ())
    {
      NS_LOG_LOGIC ("packet is not for us");
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsData ())
    {
      if (!IsAssociated ())
        {
          NS_LOG_LOGIC ("Received data frame while not associated: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (!(hdr->IsFromDs () && !hdr->IsToDs ()))
        {
          NS_LOG_LOGIC ("Received data frame not from the DS: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->GetAddr2 () != GetBssid ())
        {
          NS_LOG_LOGIC ("Received data frame not from the BSS we are associated with: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->IsQosData ())
        {
          if (hdr->IsQosAmsdu ())
            {
              NS_ASSERT (hdr->GetAddr3 () == GetBssid ());
              DeaggregateAmsduAndForward (packet, hdr);
              packet = 0;
            }
          else
            {
              ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
            }
        }
      else
        {
          ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
        }
      return;
    }
  else if (hdr->IsProbeReq ()
           || hdr->IsAssocReq ())
    {
      //This is a frame aimed at an AP, so we can safely ignore it.
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsTFBeacon ())
    {
       /*
        * Here TF beacon is just used to allocated the RUs that STAs 
        * must contend on. No other information is served
        * This must be sent by the AP well before the actual payload
        * transmissions start.
        */ 
       MgtTFBeaconHeader beacon;
       packet->RemoveHeader (beacon);
       SetTfDuration(beacon.GetTfDuration ());
       RegularWifiMac::RUAllocations alloc = beacon.GetRUAllocations ();
       for (RegularWifiMac::RUAllocations::const_iterator it = alloc.begin (); it != alloc.end (); it++)
        {
          if (it->first == GetAddress ())
           {
	     //std::cout<<"RU of "<<GetAddress () << " set to "<<it->second <<"\ttime = "<<Now ().GetMicroSeconds ()<< std::endl;
             SetRuBits (it->second);
           }
        }
    }
  else if (hdr->IsBsrAck ())
    {
        MgtBsrAckHeader bsrAck;
        packet->RemoveHeader (bsrAck);
        m_bsrAckRecvd = true;
        SetMuMode (1);
        /*
         * If BSR ack was received, then my BSR transmission was succesful
         * Reset the OCW to OCWmin and pick a new OBO
         */
        SetTfCw (GetTfCwMin ());
        PrepareForTx ();
        /*
         * After the payload transmission starts, I must not transmit 
         * another packet. So declare the channel busy until a new TF is received
         */
        Simulator::Schedule(MicroSeconds (17), &DcfManager::NotifyMaybeCcaBusyStartNow, m_dcfManagerMu[GetRuBits ()], Seconds (1));
    }
  else if (hdr->IsTF ())
    {
      ResetBsrTx ();
      m_updatedOnce = false;
      m_lastTfTxStart = m_low->CalculateTfBeaconDuration (packet, *hdr); // hack
      MgtTFHeader tf;
      packet->RemoveHeader (tf);

      RegularWifiMac::RUAllocations alloc = tf.GetRUAllocations ();
      uint32_t ulFlag = tf.GetUplinkFlag ();
      m_muUlFlag = ulFlag;
      SetTfDuration(tf.GetTfDuration ());
      m_muDlModeEnd = GetTfDuration () * GetSlot () - m_lastTfTxStart;  
      m_dcfManager->NotifyMaybeCcaBusyStartNow (m_muDlModeEnd); // Notify the DcfManager of 20 MHz PHY that the channel is busy until OFDMA mode ends
      m_muModeExpireEvent = Simulator::Schedule (m_muDlModeEnd, &StaWifiMac::StopMuMode, this); 
      Ptr<UniformRandomVariable> rv = CreateObject<UniformRandomVariable> ();
 
      if (m_firstTf)
       {
         /*
          * If this is the first TF, then I must pick a new OBO
          */
         m_firstTf = false;
         m_noSlots = rv->GetInteger (0, GetTfCw ()-1);
       }
      else if (m_noSlots == 0)
       {
         /*
          * If my slot had decremented to zero, I had transmitted.
	  * If a BSR ack was not received, then my packet must have collided
          * So double the CW and pick a new OBO
          */
         if (!m_bsrAckRecvd)
          {
            SetTfCw (2 * GetTfCw ());
            if (GetTfCw () > GetTfCwMax ())
             {
               SetTfCw (GetTfCwMax ());
             }
          }
         m_noSlots = rv->GetInteger (0, GetTfCw ()-1);
       }
      if (m_bsrAckRecvd)
       {
         m_bsrAckRecvd = false;
       }

      for (RegularWifiMac::RUAllocations::const_iterator it = alloc.begin (); it != alloc.end (); it++)
       {
         if (it->first == GetAddress ())
           {
             if (ulFlag)
               {
                 SetMuMode (1);
                 SetRuBits (it->second);
		 PrepareForTx ();
                 Simulator::Schedule(MicroSeconds (17), &DcfManager::NotifyMaybeCcaBusyStartNow, m_dcfManagerMu[GetRuBits ()], Seconds (1));
                 return;
               }
             else
               {
                 NS_LOG_UNCOND("Received Trigger Frame for DL, time = "<<Simulator::Now ().GetMicroSeconds ());
               }
           }
	  if (it->first == Mac48Address::GetBroadcast ())
           {
             std::cout<<"STA "<<m_phy->GetDevice ()->GetNode ()->GetId ()<<"\tm_noSlots =  "<<m_noSlots << "\tSelected RU = "<<GetRuBits ()<<"\tTfCw = "<<GetTfCw ()<< "\ttime = "<< Simulator::Now ().GetMicroSeconds ()<<std::endl;
             m_lastTfRespRecv = Now ();
	     m_muModeToStart = true;
             /* 
              * If the selected m_noSlots is greater than MaxTfSlots, no point
              * in scheduling the transmissions. 
              */
             if (m_noSlots < GetMaxTfSlots ())
              {
                Simulator::Schedule (m_noSlots * GetSlot(), &StaWifiMac::CheckAndCancel, this, GetRuBits ()); 
                m_triggerFrameRespEvent = Simulator::Schedule (m_noSlots * GetSlot (), &StaWifiMac::SendTriggerFrameResp, this, GetRuBits ());
              }
              /*
               * If any STA gets access to the channel on my RU in this cycle,
               * I don't need this. But if all STAs have OBO > MaxTfSlots, then
               * the channel will bever be accessed, so my OBO will not be updated
               * CancelExpiredEvents just updates the OBO in such cases.
               * Also can be used to compute the probabilities that the RU was idle 
               * in this TF cycle
               */
              Simulator::Schedule ((GetMaxTfSlots ()) * GetSlot () + GetSifs (), &StaWifiMac::CancelExpiredEvents, this);
            }
       }
    
      
    }
  else if (hdr->IsBeacon ())
    {
      MgtBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      CapabilityInformation capabilities = beacon.GetCapabilities ();
      bool goodBeacon = false;
      if (GetSsid ().IsBroadcast ()
          || beacon.GetSsid ().IsEqual (GetSsid ()))
        {
          NS_LOG_LOGIC ("Beacon is for our SSID");
          goodBeacon = true;
        }
      SupportedRates rates = beacon.GetSupportedRates ();
      bool bssMembershipSelectorMatch = false;
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          uint32_t selector = m_phy->GetBssMembershipSelector (i);
          if (rates.IsBssMembershipSelectorRate (selector))
            {
              NS_LOG_LOGIC ("Beacon is matched to our BSS membership selector");
              bssMembershipSelectorMatch = true;
            }
        }
      if (m_phy->GetNBssMembershipSelectors () > 0 && bssMembershipSelectorMatch == false)
        {
          NS_LOG_LOGIC ("No match for BSS membership selector");
          goodBeacon = false;
        }
      if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ())
        {
          NS_LOG_LOGIC ("Beacon is not for us");
          goodBeacon = false;
        }
      if (goodBeacon)
        {
          Time delay = MicroSeconds (beacon.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          SetBssid (hdr->GetAddr3 ());
          SupportedRates rates = beacon.GetSupportedRates ();
          for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
            {
              WifiMode mode = m_phy->GetMode (i);
              if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                {
                  m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                }
            }
          bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
          if (m_erpSupported)
            {
              ErpInformation erpInformation = beacon.GetErpInformation ();
              isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
              if (erpInformation.GetUseProtection () == true)
                {
                  m_stationManager->SetUseNonErpProtection (true);
                }
              else
                {
                  m_stationManager->SetUseNonErpProtection (false);
                }
              if (capabilities.IsShortSlotTime () == true)
                {
                  //enable short slot time
                  //SetSlot (MicroSeconds (9));
                  SetSlot (MicroSeconds (13));
                }
              else
                {
                  //disable short slot time
                  SetSlot (MicroSeconds (20));
                }
            }
          if (m_qosSupported)
            {
              bool qosSupported = false;
              EdcaParameterSet edcaParameters = beacon.GetEdcaParameterSet ();
              if (edcaParameters.IsQosSupported ())
                {
                  qosSupported = true;
                  //The value of the TXOP Limit field is specified as an unsigned integer, with the least significant octet transmitted first, in units of 32 μs.
                  SetEdcaParameters (AC_BE, edcaParameters.GetBeCWmin (), edcaParameters.GetBeCWmax (), edcaParameters.GetBeAifsn (), 32 * MicroSeconds (edcaParameters.GetBeTXOPLimit ()));
                  SetEdcaParameters (AC_BK, edcaParameters.GetBkCWmin (), edcaParameters.GetBkCWmax (), edcaParameters.GetBkAifsn (), 32 * MicroSeconds (edcaParameters.GetBkTXOPLimit ()));
                  SetEdcaParameters (AC_VI, edcaParameters.GetViCWmin (), edcaParameters.GetViCWmax (), edcaParameters.GetViAifsn (), 32 * MicroSeconds (edcaParameters.GetViTXOPLimit ()));
                  SetEdcaParameters (AC_VO, edcaParameters.GetVoCWmin (), edcaParameters.GetVoCWmax (), edcaParameters.GetVoAifsn (), 32 * MicroSeconds (edcaParameters.GetVoTXOPLimit ()));
                }
              m_stationManager->SetQosSupport (hdr->GetAddr2 (), qosSupported);
            }
          if (m_htSupported)
            {
              HtCapabilities htCapabilities = beacon.GetHtCapabilities ();
              if (!htCapabilities.IsSupportedMcs (0))
                {
                  m_stationManager->RemoveAllSupportedMcs (hdr->GetAddr2 ());
                }
              else
                {
                  m_stationManager->AddStationHtCapabilities (hdr->GetAddr2 (), htCapabilities);
                  HtOperation htOperation = beacon.GetHtOperation ();
                  if (htOperation.GetNonGfHtStasPresent ())
                    {
                      m_stationManager->SetUseGreenfieldProtection (true);
                    }
                  else
                    {
                      m_stationManager->SetUseGreenfieldProtection (false);
                    }
                  if (!m_vhtSupported && GetRifsSupported () && htOperation.GetRifsMode ())
                    {
                      m_stationManager->SetRifsPermitted (true);
                    }
                  else
                    {
                      m_stationManager->SetRifsPermitted (false);
                    }
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HT && htCapabilities.IsSupportedMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                        }
                    }
                }
            }
          if (m_vhtSupported)
            {
              VhtCapabilities vhtCapabilities = beacon.GetVhtCapabilities ();
              //we will always fill in RxHighestSupportedLgiDataRate field at TX, so this can be used to check whether it supports VHT
              if (vhtCapabilities.GetRxHighestSupportedLgiDataRate () > 0)
                {
                  m_stationManager->AddStationVhtCapabilities (hdr->GetAddr2 (), vhtCapabilities);
                  VhtOperation vhtOperation = beacon.GetVhtOperation ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_VHT && vhtCapabilities.IsSupportedRxMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                        }
                    }
                }
            }
          if (m_heSupported)
            {
              HeCapabilities heCapabilities = beacon.GetHeCapabilities ();
              //todo: once we support non constant rate managers, we should add checks here whether HE is supported by the peer
              m_stationManager->AddStationHeCapabilities (hdr->GetAddr2 (), heCapabilities);
              for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                {
                  WifiMode mcs = m_phy->GetMcs (i);
                  if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HE && heCapabilities.IsSupportedRxMcs (mcs.GetMcsValue ()))
                    {
                      m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                    }
                }
            }
          m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
          m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
        }
      if (goodBeacon && m_state == BEACON_MISSED)
        {
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsProbeResp ())
    {
      if (m_state == WAIT_PROBE_RESP)
        {
          MgtProbeResponseHeader probeResp;
          packet->RemoveHeader (probeResp);
          CapabilityInformation capabilities = probeResp.GetCapabilities ();
          if (!probeResp.GetSsid ().IsEqual (GetSsid ()))
            {
              //not a probe resp for our ssid.
              return;
            }
          SupportedRates rates = probeResp.GetSupportedRates ();
          for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
            {
              uint32_t selector = m_phy->GetBssMembershipSelector (i);
              if (!rates.IsSupportedRate (selector))
                {
                  return;
                }
            }
          for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
            {
              WifiMode mode = m_phy->GetMode (i);
              if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                {
                  m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                  if (rates.IsBasicRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                    {
                      m_stationManager->AddBasicMode (mode);
                    }
                }
            }

          bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
          if (m_erpSupported)
            {
              bool isErpAllowed = false;
              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                {
                  WifiMode mode = m_phy->GetMode (i);
                  if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM && rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                    {
                      isErpAllowed = true;
                      break;
                    }
                }
              if (!isErpAllowed)
                {
                  //disable short slot time and set cwMin to 31
                  SetSlot (MicroSeconds (20));
                  ConfigureContentionWindow (31, 1023);
                }
              else
                {
                  ErpInformation erpInformation = probeResp.GetErpInformation ();
                  isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
                  if (m_stationManager->GetShortSlotTimeEnabled ())
                    {
                      //enable short slot time
                      //SetSlot (MicroSeconds (9));
                      SetSlot (MicroSeconds (13));
                    }
                  else
                    {
                      //disable short slot time
                      SetSlot (MicroSeconds (20));
                    }
                  ConfigureContentionWindow (15, 1023);
                }
            }
          m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
          m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
          SetBssid (hdr->GetAddr3 ());
          Time delay = MicroSeconds (probeResp.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          if (m_probeRequestEvent.IsRunning ())
            {
              m_probeRequestEvent.Cancel ();
            }
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsAssocResp ())
    {
      if (m_state == WAIT_ASSOC_RESP)
        {
          MgtAssocResponseHeader assocResp;
          packet->RemoveHeader (assocResp);
          if (m_assocRequestEvent.IsRunning ())
            {
              m_assocRequestEvent.Cancel ();
            }
          if (assocResp.GetStatusCode ().IsSuccess ())
            {
              SetState (ASSOCIATED);
              NS_LOG_DEBUG ("assoc completed");
              CapabilityInformation capabilities = assocResp.GetCapabilities ();
              SupportedRates rates = assocResp.GetSupportedRates ();
              bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
              if (m_erpSupported)
                {
                  bool isErpAllowed = false;
                  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                    {
                      WifiMode mode = m_phy->GetMode (i);
                      if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM && rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                        {
                          isErpAllowed = true;
                          break;
                        }
                    }
                  if (!isErpAllowed)
                    {
                      //disable short slot time and set cwMin to 31
                      SetSlot (MicroSeconds (20));
                      ConfigureContentionWindow (31, 1023);
                    }
                  else
                    {
                      ErpInformation erpInformation = assocResp.GetErpInformation ();
                      isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
                      if (m_stationManager->GetShortSlotTimeEnabled ())
                        {
                          //enable short slot time
                          //SetSlot (MicroSeconds (9));
                          SetSlot (MicroSeconds (13));
                        }
                      else
                        {
                          //disable short slot time
                          SetSlot (MicroSeconds (20));
                        }
                      ConfigureContentionWindow (15, 1023);
                    }
                }
              m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
              m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
              if (m_qosSupported)
                {
                  bool qosSupported = false;
                  EdcaParameterSet edcaParameters = assocResp.GetEdcaParameterSet ();
                  if (edcaParameters.IsQosSupported ())
                    {
                      qosSupported = true;
                      //The value of the TXOP Limit field is specified as an unsigned integer, with the least significant octet transmitted first, in units of 32 μs.
                      SetEdcaParameters (AC_BE, edcaParameters.GetBeCWmin (), edcaParameters.GetBeCWmax (), edcaParameters.GetBeAifsn (), 32 * MicroSeconds (edcaParameters.GetBeTXOPLimit ()));
                      SetEdcaParameters (AC_BK, edcaParameters.GetBkCWmin (), edcaParameters.GetBkCWmax (), edcaParameters.GetBkAifsn (), 32 * MicroSeconds (edcaParameters.GetBkTXOPLimit ()));
                      SetEdcaParameters (AC_VI, edcaParameters.GetViCWmin (), edcaParameters.GetViCWmax (), edcaParameters.GetViAifsn (), 32 * MicroSeconds (edcaParameters.GetViTXOPLimit ()));
                      SetEdcaParameters (AC_VO, edcaParameters.GetVoCWmin (), edcaParameters.GetVoCWmax (), edcaParameters.GetVoAifsn (), 32 * MicroSeconds (edcaParameters.GetVoTXOPLimit ()));
                    }
                  m_stationManager->SetQosSupport (hdr->GetAddr2 (), qosSupported);
                }
              if (m_htSupported)
                {
                  HtCapabilities htCapabilities = assocResp.GetHtCapabilities ();
                  if (!htCapabilities.IsSupportedMcs (0))
                    {
                      m_stationManager->RemoveAllSupportedMcs (hdr->GetAddr2 ());
                    }
                  else
                    {
                      m_stationManager->AddStationHtCapabilities (hdr->GetAddr2 (), htCapabilities);
                      HtOperation htOperation = assocResp.GetHtOperation ();
                      if (htOperation.GetNonGfHtStasPresent ())
                        {
                          m_stationManager->SetUseGreenfieldProtection (true);
                        }
                      else
                        {
                          m_stationManager->SetUseGreenfieldProtection (false);
                        }
                      if (!m_vhtSupported && GetRifsSupported () && htOperation.GetRifsMode ())
                        {
                          m_stationManager->SetRifsPermitted (true);
                        }
                      else
                        {
                          m_stationManager->SetRifsPermitted (false);
                        }
                    }
                }
              if (m_vhtSupported)
                {
                  VhtCapabilities vhtCapabilities = assocResp.GetVhtCapabilities ();
                  //we will always fill in RxHighestSupportedLgiDataRate field at TX, so this can be used to check whether it supports VHT
                  if (vhtCapabilities.GetRxHighestSupportedLgiDataRate () > 0)
                    {
                      m_stationManager->AddStationVhtCapabilities (hdr->GetAddr2 (), vhtCapabilities);
                      VhtOperation vhtOperation = assocResp.GetVhtOperation ();
                    }
                }
              if (m_heSupported)
                {
                  HeCapabilities hecapabilities = assocResp.GetHeCapabilities ();
                  //todo: once we support non constant rate managers, we should add checks here whether HE is supported by the peer
                  m_stationManager->AddStationHeCapabilities (hdr->GetAddr2 (), hecapabilities);
                }
              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                {
                  WifiMode mode = m_phy->GetMode (i);
                  if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                    {
                      m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                      if (rates.IsBasicRate (mode.GetDataRate (m_phy->GetChannelWidth ())))
                        {
                          m_stationManager->AddBasicMode (mode);
                        }
                    }
                }
              if (m_htSupported)
                {
                  HtCapabilities htCapabilities = assocResp.GetHtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HT && htCapabilities.IsSupportedMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (m_vhtSupported)
                {
                  VhtCapabilities vhtcapabilities = assocResp.GetVhtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_VHT && vhtcapabilities.IsSupportedRxMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (m_heSupported)
                {
                  HeCapabilities heCapabilities = assocResp.GetHeCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HE && heCapabilities.IsSupportedRxMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (!m_linkUp.IsNull ())
                {
                  m_linkUp ();
                }
            }
          else
            {
              NS_LOG_DEBUG ("assoc refused");
              SetState (REFUSED);
            }
        }
      return;
    }

  //Invoke the receive handler of our parent class to deal with any
  //other frames. Specifically, this will handle Block Ack-related
  //Management Action frames.
  RegularWifiMac::Receive (packet, hdr);
}

SupportedRates
StaWifiMac::GetSupportedRates (void) const
{
  SupportedRates rates;
  if (m_htSupported || m_vhtSupported || m_heSupported)
    {
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          rates.AddBssMembershipSelectorRate (m_phy->GetBssMembershipSelector (i));
        }
    }
  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
    {
      WifiMode mode = m_phy->GetMode (i);
      uint64_t modeDataRate = mode.GetDataRate (m_phy->GetChannelWidth ());
      NS_LOG_DEBUG ("Adding supported rate of " << modeDataRate);
      rates.AddSupportedRate (modeDataRate);
    }
  return rates;
}

CapabilityInformation
StaWifiMac::GetCapabilities (void) const
{
  CapabilityInformation capabilities;
  capabilities.SetShortPreamble (m_phy->GetShortPlcpPreambleSupported () || m_erpSupported);
  capabilities.SetShortSlotTime (GetShortSlotTimeSupported () && m_erpSupported);
  return capabilities;
}

void
StaWifiMac::SetState (MacState value)
{
  if (value == ASSOCIATED
      && m_state != ASSOCIATED)
    {
      m_assocLogger (GetBssid ());
    }
  else if (value != ASSOCIATED
           && m_state == ASSOCIATED)
    {
      m_deAssocLogger (GetBssid ());
    }
  m_state = value;
}

void
StaWifiMac::SetEdcaParameters (AcIndex ac, uint8_t cwMin, uint8_t cwMax, uint8_t aifsn, Time txopLimit)
{
  Ptr<EdcaTxopN> edca = m_edca.find (ac)->second;
  edca->SetMinCw (cwMin);
  edca->SetMaxCw (cwMax);
  edca->SetAifsn (aifsn);
  edca->SetTxopLimit (txopLimit);

  for (uint32_t i = 0; i<9; i++)
    {
      Ptr<EdcaTxopN> edcaMu = m_edcaMu[i].find (ac)->second;
      edcaMu->SetMinCw (0);
      edcaMu->SetMaxCw (0);
      edcaMu->SetAifsn (0);
      edcaMu->SetTxopLimit (Seconds(0));
      
    }
}

void
StaWifiMac::PrepareForTx (void)
{
  for (uint32_t ac = 0; ac < 8; ac++)
   {
     m_edcaMu[GetRuBits ()][QosUtilsMapTidToAc(ac)]->SetAifsn (0);
     m_edcaMu[GetRuBits ()][QosUtilsMapTidToAc(ac)]->SetMinCw (0);
     m_edcaMu[GetRuBits ()][QosUtilsMapTidToAc(ac)]->SetMaxCw (0);
   }
  m_dcfManagerMu[GetRuBits ()]->UpdateBusyDuration ();
  for (uint32_t i = 0; i < 8; i++)
   {
     m_edcaMu[GetRuBits ()][QosUtilsMapTidToAc(i)]->StartAccessIfNeeded ();
   }
}

void 
StaWifiMac::SetTfDuration (uint32_t tfDuration)
{
  m_tfDuration = tfDuration;
}

uint32_t 
StaWifiMac::GetTfDuration (void) const
{
  return m_tfDuration;
}

void
StaWifiMac::StopMuMode (void)
{
  NS_LOG_UNCOND ("Inside StaWifiMac:Stopping MuMode, time = "<<Simulator::Now ().GetMicroSeconds ());
  SetMuMode (0);
  for (uint32_t ru = 0; ru < 9; ru++)
   {
     m_dcaMu[ru]->StopMuMode ();
     m_dcfManagerMu[ru]->NotifyMaybeCcaBusyStartNow (Seconds(1));
     for (uint32_t ac = 0; ac < 8; ac++)
      {
	m_edcaMu[ru][QosUtilsMapTidToAc(ac)]->StopMuMode ();
      }
   }
}
} //namespace ns3
