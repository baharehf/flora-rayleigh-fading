//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#include "NetworkServerApp.h"
//#include "inet/networklayer/ipv4/IPv4Datagram.h"
//#include "inet/networklayer/contract/ipv4/IPv4ControlInfo.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/transportlayer/common/L4PortTag_m.h"
#include "inet/transportlayer/contract/udp/UdpControlInfo_m.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/common/ModuleAccess.h"
#include "inet/applications/base/ApplicationPacket_m.h"

#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/ipv4/Ipv4Header_m.h"

#include <omnetpp.h>

namespace flora {

Define_Module(NetworkServerApp);


void NetworkServerApp::initialize(int stage)
{
    if (stage == 0) {
        ASSERT(recvdPackets.size()==0);
        LoRa_ServerPacketReceived = registerSignal("LoRa_ServerPacketReceived");
        localPort = par("localPort");
        destPort = par("destPort");
        adrMethod = par("adrMethod").stdstringValue();
    } else if (stage == INITSTAGE_APPLICATION_LAYER) {
        startUDP();
        getSimulation()->getSystemModule()->subscribe("LoRa_AppPacketSent", this);
        evaluateADRinServer = par("evaluateADRinServer");
        adrDeviceMargin = par("adrDeviceMargin");
        receivedRSSI.setName("Received RSSI");
        totalReceivedPackets = 0;
        for(int i=0;i<6;i++)
        {
            counterUniqueReceivedPacketsPerSF[i] = 0;
            counterOfSentPacketsFromNodesPerSF[i] = 0;
        }
    }
}


void NetworkServerApp::startUDP()
{
    socket.setOutputGate(gate("socketOut"));
    const char *localAddress = par("localAddress");
    socket.bind(*localAddress ? L3AddressResolver().resolve(localAddress) : L3Address(), localPort);
}


void NetworkServerApp::handleMessage(cMessage *msg)
{
    if (msg->arrivedOn("socketIn")) {
        auto pkt = check_and_cast<Packet *>(msg);
        const auto &frame  = pkt->peekAtFront<LoRaMacFrame>();
        if (frame == nullptr)
            throw cRuntimeError("Header error type");
        //LoRaMacFrame *frame = check_and_cast<LoRaMacFrame *>(msg);
        if (simTime() >= getSimulation()->getWarmupPeriod())
        {
            totalReceivedPackets++;
        }
        updateKnownNodes(pkt);
        processLoraMACPacket(pkt);
    }
    else if(msg->isSelfMessage()) {
        processScheduledPacket(msg);
    }
}

void NetworkServerApp::processLoraMACPacket(Packet *pk)
{
    const auto & frame = pk->peekAtFront<LoRaMacFrame>();
    if(isPacketProcessed(frame))
    {
        delete pk;
        return;
    }
    addPktToProcessingTable(pk);
}

void NetworkServerApp::finish()
{
    recordScalar("LoRa_NS_DER", double(counterUniqueReceivedPackets)/counterOfSentPacketsFromNodes);
    for(uint i=0;i<knownNodes.size();i++)
    {
        delete knownNodes[i].historyAllSNIR;
        delete knownNodes[i].historyAllRSSI;
        delete knownNodes[i].receivedSeqNumber;
        delete knownNodes[i].calculatedSNRmargin;
        delete knownNodes[i].SNRmVector;
        recordScalar("Send ADR for node", knownNodes[i].numberOfSentADRPackets);
        // Recording SNR for each node as a Scalar
        for(auto snrm : knownNodes[i].historySNRm)
        {
            std::string recordName = "SNRm for Node " + std::to_string(knownNodes[i].nodeNumber);
            recordScalar(recordName.c_str(), snrm);
        }
    }
    for (std::map<int,int>::iterator it=numReceivedPerNode.begin(); it != numReceivedPerNode.end(); ++it)
    {
        const std::string stringScalar = "numReceivedFromNode " + std::to_string(it->first);
        recordScalar(stringScalar.c_str(), it->second);
    }

    receivedRSSI.recordAs("receivedRSSI");
    recordScalar("totalReceivedPackets", totalReceivedPackets);

    while(!receivedPackets.empty()) {
        receivedPackets.back().endOfWaiting->removeControlInfo();
        delete receivedPackets.back().rcvdPacket;
        if (receivedPackets.back().endOfWaiting && receivedPackets.back().endOfWaiting->isScheduled()) {
            cancelAndDelete(receivedPackets.back().endOfWaiting);
        }
        else
            delete receivedPackets.back().endOfWaiting;
        receivedPackets.pop_back();
    }

    knownNodes.clear();
    receivedPackets.clear();

    recordScalar("counterUniqueReceivedPacketsPerSF SF7", counterUniqueReceivedPacketsPerSF[0]);
    recordScalar("counterUniqueReceivedPacketsPerSF SF8", counterUniqueReceivedPacketsPerSF[1]);
    recordScalar("counterUniqueReceivedPacketsPerSF SF9", counterUniqueReceivedPacketsPerSF[2]);
    recordScalar("counterUniqueReceivedPacketsPerSF SF10", counterUniqueReceivedPacketsPerSF[3]);
    recordScalar("counterUniqueReceivedPacketsPerSF SF11", counterUniqueReceivedPacketsPerSF[4]);
    recordScalar("counterUniqueReceivedPacketsPerSF SF12", counterUniqueReceivedPacketsPerSF[5]);
    if (counterOfSentPacketsFromNodesPerSF[0] > 0)
        recordScalar("DER SF7", double(counterUniqueReceivedPacketsPerSF[0]) / counterOfSentPacketsFromNodesPerSF[0]);
    else
        recordScalar("DER SF7", 0);

    if (counterOfSentPacketsFromNodesPerSF[1] > 0)
        recordScalar("DER SF8", double(counterUniqueReceivedPacketsPerSF[1]) / counterOfSentPacketsFromNodesPerSF[1]);
    else
        recordScalar("DER SF8", 0);

    if (counterOfSentPacketsFromNodesPerSF[2] > 0)
        recordScalar("DER SF9", double(counterUniqueReceivedPacketsPerSF[2]) / counterOfSentPacketsFromNodesPerSF[2]);
    else
        recordScalar("DER SF9", 0);

    if (counterOfSentPacketsFromNodesPerSF[3] > 0)
        recordScalar("DER SF10", double(counterUniqueReceivedPacketsPerSF[3]) / counterOfSentPacketsFromNodesPerSF[3]);
    else
        recordScalar("DER SF10", 0);

    if (counterOfSentPacketsFromNodesPerSF[4] > 0)
        recordScalar("DER SF11", double(counterUniqueReceivedPacketsPerSF[4]) / counterOfSentPacketsFromNodesPerSF[4]);
    else
        recordScalar("DER SF11", 0);

    if (counterOfSentPacketsFromNodesPerSF[5] > 0)
        recordScalar("DER SF12", double(counterUniqueReceivedPacketsPerSF[5]) / counterOfSentPacketsFromNodesPerSF[5]);
    else
        recordScalar("DER SF12", 0);
}

bool NetworkServerApp::isPacketProcessed(const Ptr<const LoRaMacFrame> &pkt)
{
    for(const auto & elem : knownNodes) {
        if(elem.srcAddr == pkt->getTransmitterAddress()) {
            if(elem.lastSeqNoProcessed > pkt->getSequenceNumber()) return true;
        }
    }
    return false;
}


std::map<L3Address, int> addressToNodeIndex;
int getNodeIndex(const L3Address& address) {
    static int nextIndex = 0;
    if (addressToNodeIndex.find(address) == addressToNodeIndex.end()) {
        addressToNodeIndex[address] = nextIndex++;
    }
    return addressToNodeIndex[address];
}


void NetworkServerApp::updateKnownNodes(Packet* pkt)
{
    const auto & frame = pkt->peekAtFront<LoRaMacFrame>();
    bool nodeExist = false;
    for(auto &elem : knownNodes)
    {
        if(elem.srcAddr == frame->getTransmitterAddress()) {
            nodeExist = true;
            if(elem.lastSeqNoProcessed < frame->getSequenceNumber()) {
                elem.lastSeqNoProcessed = frame->getSequenceNumber();
            }
            break;
        }
    }

    if(nodeExist == false)
    {
        knownNode newNode;
        newNode.srcAddr= frame->getTransmitterAddress();
        newNode.lastSeqNoProcessed = frame->getSequenceNumber();
        newNode.framesFromLastADRCommand = 0;
        newNode.numberOfSentADRPackets = 0;
        //newNode.historyAllSNIR = new cOutVector;
        //newNode.historyAllSNIR->setName("Vector of SNIR per node");
        //newNode.historyAllSNIR->record(pkt->getSNIR());
        //newNode.historyAllSNIR->record(math::fraction2dB(frame->getSNIR()));
        int nodeIndex = getNodeIndex(frame->getTransmitterAddress());
        newNode.historyAllSNIR = new cOutVector;
        std::string snirVectorName = "SNIR_GW for Node " + std::to_string(nodeIndex);
        newNode.historyAllSNIR->setName(snirVectorName.c_str());
        newNode.historyAllSNIR->record(math::fraction2dB(frame->getSNIR()));

        newNode.historyAllRSSI = new cOutVector;
        newNode.historyAllRSSI->setName("Vector of RSSI per node");
        newNode.historyAllRSSI->record(frame->getRSSI());
        newNode.receivedSeqNumber = new cOutVector;
        newNode.receivedSeqNumber->setName("Received Sequence number");
        newNode.calculatedSNRmargin = new cOutVector;
        newNode.calculatedSNRmargin->setName("Calculated SNRmargin in ADR");
        //std::string snrmarginName = "SNRmargin for Node " + std::to_string(nodeIndex);
        //newNode.calculatedSNRmargin->setName(snrmarginName.c_str());
        //newNode.snrMarginVector = new cOutVector;
        //newNode.snrMarginVector->setName("SNRm per Node");
        newNode.SNRmVector = new cOutVector;
        //std::string vectorName = "SNRm for Node " + std::to_string(newNode.srcAddr.getInt());
       //nt nodeIndex = getNodeIndex(frame->getTransmitterAddress());
        std::string vectorName = "SNRm for Node " + std::to_string(nodeIndex);
        newNode.SNRmVector->setName(vectorName.c_str());

        //newNode.snrMarginVector = new cOutVector(std::string("SNRm for Node ") + std::to_string(newNode.srcAddr));
        //newNode.snrMarginVector->setName("SNRm_Vector");

        //int nodeIndex = getNodeIndex(frame->getTransmitterAddress());
        //std::string vectorName = "Calculated SNRmargin in ADR for Node " + std::to_string(nodeIndex);
        //newNode.calculatedSNRmargin->setName(vectorName.c_str());

        knownNodes.push_back(newNode);
    }
}

void NetworkServerApp::addPktToProcessingTable(Packet* pkt)
{
    const auto & frame = pkt->peekAtFront<LoRaMacFrame>();
    bool packetExists = false;
    for(auto &elem : receivedPackets)
    {
        const auto &frameAux = elem.rcvdPacket->peekAtFront<LoRaMacFrame>();
        if(frameAux->getTransmitterAddress() == frame->getTransmitterAddress() && frameAux->getSequenceNumber() == frame->getSequenceNumber())
        {
            packetExists = true;
            const auto& networkHeader = getNetworkProtocolHeader(pkt);
            const L3Address& gwAddress = networkHeader->getSourceAddress();
            elem.possibleGateways.emplace_back(gwAddress, frame->getSNIR(), frame->getRSSI());
            delete pkt;
            break;
        }
    }
    if(packetExists == false)
    {
        receivedPacket rcvPkt;
        rcvPkt.rcvdPacket = pkt;
        rcvPkt.endOfWaiting = new cMessage("endOfWaitingWindow");
        rcvPkt.endOfWaiting->setControlInfo(pkt);
        const auto& networkHeader = getNetworkProtocolHeader(pkt);
        const L3Address& gwAddress = networkHeader->getSourceAddress();
        rcvPkt.possibleGateways.emplace_back(gwAddress, frame->getSNIR(), frame->getRSSI());
        EV << "Added " << gwAddress << " " << frame->getSNIR() << " " << frame->getRSSI() << endl;
        scheduleAt(simTime() + 1.2, rcvPkt.endOfWaiting);
        receivedPackets.push_back(rcvPkt);
    }
}

void NetworkServerApp::processScheduledPacket(cMessage* selfMsg)
{
    auto pkt = check_and_cast<Packet *>(selfMsg->removeControlInfo());
    const auto & frame = pkt->peekAtFront<LoRaMacFrame>();

    if (simTime() >= getSimulation()->getWarmupPeriod())
    {
        counterUniqueReceivedPacketsPerSF[frame->getLoRaSF()-7]++;
    }
    L3Address pickedGateway;
    double SNIRinGW = -99999999999;
    double RSSIinGW = -99999999999;
    int packetNumber;
    int nodeNumber;
    for(uint i=0;i<receivedPackets.size();i++)
    {
        const auto &frameAux = receivedPackets[i].rcvdPacket->peekAtFront<LoRaMacFrame>();
        if(frameAux->getTransmitterAddress() == frame->getTransmitterAddress() && frameAux->getSequenceNumber() == frame->getSequenceNumber())        {
            packetNumber = i;
            nodeNumber = frame->getTransmitterAddress().getInt();
            if (numReceivedPerNode.count(nodeNumber-1)>0)
            {
                ++numReceivedPerNode[nodeNumber-1];
            } else {
                numReceivedPerNode[nodeNumber-1] = 1;
            }

            for(uint j=0;j<receivedPackets[i].possibleGateways.size();j++)
            {
                if(SNIRinGW < std::get<1>(receivedPackets[i].possibleGateways[j]))
                {
                    RSSIinGW = std::get<2>(receivedPackets[i].possibleGateways[j]);
                    SNIRinGW = std::get<1>(receivedPackets[i].possibleGateways[j]);
                    pickedGateway = std::get<0>(receivedPackets[i].possibleGateways[j]);
                }
            }
            // Record SNIR and RSSI for this node regardless of ADR settings
//            for (auto& node : knownNodes) {
//                if (node.srcAddr == frame->getTransmitterAddress()) {
//                    node.historyAllSNIR->record(SNIRinGW);
//                    node.historyAllRSSI->record(RSSIinGW);
//                    node.receivedSeqNumber->record(frame->getSequenceNumber());
//                    break;
//                }
//            }
        }
    }
    emit(LoRa_ServerPacketReceived, true);
    if (simTime() >= getSimulation()->getWarmupPeriod())
    {
        counterUniqueReceivedPackets++;
    }
    receivedRSSI.collect(frame->getRSSI());
    if(evaluateADRinServer)
    {
        evaluateADR(pkt, pickedGateway, SNIRinGW, RSSIinGW);
    }
    delete receivedPackets[packetNumber].rcvdPacket;
    delete selfMsg;
    receivedPackets.erase(receivedPackets.begin()+packetNumber);
}

double calculatePercentileFromHistogram(const omnetpp::cHistogram& histogram, double percentile) {
    std::vector<double> data;

    for (int i = 0; i < histogram.getNumBins(); ++i) {
        double binStart = histogram.getBinEdge(i);
        double binEnd = histogram.getBinEdge(i + 1);
        long count = histogram.getBinValue(i);
        double binValue = (binStart + binEnd) / 2.0;

        for (long j = 0; j < count; ++j) {
            data.push_back(binValue);
        }
    }

    if (data.empty()) {
        EV << "Warning: Histogram is empty. Returning default value.\n";
        return 0.0;
    }

    std::sort(data.begin(), data.end());
    int index = static_cast<int>(ceil(percentile / 100.0 * data.size())) - 1;
    index = std::max(0, std::min(index, (int)data.size() - 1));

    return data[index];
}


void NetworkServerApp::evaluateADR(Packet* pkt, L3Address pickedGateway, double SNIRinGW, double RSSIinGW)
{
    bool sendADR = false;
    bool sendADRAckRep = false;
    double SNRm; //needed for ADR
    int nodeIndex;
    NS_increaseSF = par("NS_increaseSF").boolValue();
    pkt->trimFront();
    auto frame = pkt->removeAtFront<LoRaMacFrame>();

    const auto & rcvAppPacket = pkt->peekAtFront<LoRaAppPacket>();

    if(rcvAppPacket->getOptions().getADRACKReq())
    {
        sendADRAckRep = true;
    }

    for(uint i=0;i<knownNodes.size();i++)
    {
        if(knownNodes[i].srcAddr == frame->getTransmitterAddress())
        {
            knownNodes[i].adrListSNIR.push_back(SNIRinGW);
            knownNodes[i].historyAllSNIR->record(SNIRinGW);
            knownNodes[i].historyAllRSSI->record(RSSIinGW);
            knownNodes[i].receivedSeqNumber->record(frame->getSequenceNumber());
            if(knownNodes[i].adrListSNIR.size() == 20) knownNodes[i].adrListSNIR.pop_front();
            knownNodes[i].framesFromLastADRCommand++;
            EV << "Recording SNRm value: " << SNRm << " for node " << i << endl;
            if(knownNodes[i].framesFromLastADRCommand == 20 || sendADRAckRep == true)
            {
                nodeIndex = i;
                knownNodes[i].framesFromLastADRCommand = 0;
                sendADR = true;
                if(adrMethod == "max")
                {
                    SNRm = *max_element(knownNodes[i].adrListSNIR.begin(), knownNodes[i].adrListSNIR.end());
                }
                if(adrMethod == "min")
                {
                    SNRm = *min_element(knownNodes[i].adrListSNIR.begin(), knownNodes[i].adrListSNIR.end());
                }
                if(adrMethod == "avg")
                {
                    double totalSNR = 0;
                    int numberOfFields = 0;
                    for (std::list<double>::iterator it=knownNodes[i].adrListSNIR.begin(); it != knownNodes[i].adrListSNIR.end(); ++it)
                    {
                        totalSNR+=*it;
                        numberOfFields++;
                    }
                    SNRm = totalSNR/numberOfFields;
                }
                if(adrMethod == "percentile")
                {
                    for (std::list<double>::iterator it=knownNodes[i].adrListSNIR.begin(); it != knownNodes[i].adrListSNIR.end(); ++it)
                    {
                        knownNodes[i].snirHistogram.collect(*it);
                    }
                    SNRm = calculatePercentileFromHistogram(knownNodes[i].snirHistogram, 2.5);
                }
                //EV << "[MSDebug] SNRm value is: " << SNRm << endl;
                knownNodes[i].SNRmVector->record(SNRm);     // Record SNRm as a vector
                //knownNodes[i].historySNRm.push_back(SNRm);  // Record SNRm as a scalar
                knownNodes[i].nodeNumber = frame->getTransmitterAddress().getInt(); // Set node number
            }

        }
    }

    if(sendADR || sendADRAckRep)
    {
        auto mgmtPacket = makeShared<LoRaAppPacket>();
        mgmtPacket->setMsgType(TXCONFIG);

        if(sendADR)
        {
            double SNRmargin;
            double requiredSNR;

            if(frame->getLoRaSF() == 7) requiredSNR = -7.5;
            if(frame->getLoRaSF() == 8) requiredSNR = -10;
            if(frame->getLoRaSF() == 9) requiredSNR = -12.5;
            if(frame->getLoRaSF() == 10) requiredSNR = -15;
            if(frame->getLoRaSF() == 11) requiredSNR = -17.5;
            if(frame->getLoRaSF() == 12) requiredSNR = -20;

            SNRmargin = SNRm - requiredSNR - adrDeviceMargin;
            knownNodes[nodeIndex].calculatedSNRmargin->record(SNRmargin);

            int Nstep = round(SNRmargin/3);
            LoRaOptions newOptions;

            // Increase the data rate with each step
            int calculatedSF = frame->getLoRaSF();
            while(Nstep > 0 && calculatedSF > 7)
            {
                calculatedSF--;
                Nstep--;
            }

            // Decrease the Tx power by 3 for each step, until min reached
            double calculatedPowerdBm = math::mW2dBmW(frame->getLoRaTP()) + 30;
            while(Nstep > 0 && calculatedPowerdBm > 2)
            {
                calculatedPowerdBm-=3;
                Nstep--;
            }
            if(calculatedPowerdBm < 2) calculatedPowerdBm = 2;

            // Increase the Tx power by 3 for each step, until max reached
            while(Nstep < 0 && calculatedPowerdBm < 14)
            {
                calculatedPowerdBm+=3;
                Nstep++;
            }
            if(calculatedPowerdBm > 14) calculatedPowerdBm = 14;

            //If TP is already at maximum and Nstep is still negative, INCREASE SF
            if(NS_increaseSF)
            {
                if (Nstep < 0 && calculatedPowerdBm == 14)
                {
                    while (Nstep < 0 && calculatedSF < 12) {
                        calculatedSF++;
                        Nstep++;
                    }
                }
            }

            newOptions.setLoRaSF(calculatedSF);
            newOptions.setLoRaTP(calculatedPowerdBm);
            EV << calculatedSF << endl;
            EV << calculatedPowerdBm << endl;
            mgmtPacket->setOptions(newOptions);
        }

        if(simTime() >= getSimulation()->getWarmupPeriod() && sendADR == true)
        {
            knownNodes[nodeIndex].numberOfSentADRPackets++;
        }

        auto frameToSend = makeShared<LoRaMacFrame>();
        frameToSend->setChunkLength(B(par("headerLength").intValue()));

      //  LoRaMacFrame *frameToSend = new LoRaMacFrame("ADRPacket");

        //frameToSend->encapsulate(mgmtPacket);
        frameToSend->setReceiverAddress(frame->getTransmitterAddress());
        //FIXME: What value to set for LoRa TP
        //frameToSend->setLoRaTP(pkt->getLoRaTP());
        frameToSend->setLoRaTP(math::dBmW2mW(14));
        frameToSend->setLoRaCF(frame->getLoRaCF());
        frameToSend->setLoRaSF(frame->getLoRaSF());
        frameToSend->setLoRaBW(frame->getLoRaBW());

        auto pktAux = new Packet("ADRPacket");
        mgmtPacket->setChunkLength(B(par("headerLength").intValue()));

        pktAux->insertAtFront(mgmtPacket);
        pktAux->insertAtFront(frameToSend);
        socket.sendTo(pktAux, pickedGateway, destPort);

    }
    //delete pkt;
}

void NetworkServerApp::receiveSignal(cComponent *source, simsignal_t signalID, intval_t value, cObject *details)
{
    if (simTime() >= getSimulation()->getWarmupPeriod())
    {
        counterOfSentPacketsFromNodes++;
        counterOfSentPacketsFromNodesPerSF[value-7]++;
    }
}

} //namespace inet
