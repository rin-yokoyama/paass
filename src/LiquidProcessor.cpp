/** \file LiquidProcessor.cpp
 *
 * implementation for scintillator processor
 */
#include <vector>
#include <sstream>

#include <cmath>

#include "DammPlotIds.hpp"
#include "DetectorDriver.hpp"
#include "RawEvent.hpp"
#include "LiquidProcessor.hpp"
#include "Trace.hpp"

using namespace std;
using namespace dammIds::scint::liquid;

namespace dammIds {
    namespace scint {
        namespace liquid {
            const int DD_TQDCLIQUID       = 0;
            const int DD_MAXLIQUID        = 1;
            const int DD_DISCRIM          = 2;
            const int DD_TOFLIQUID        = 3;
            const int DD_TRCLIQUID        = 4;
            const int DD_TQDCVSDISCRIM    = 5;
            const int DD_TOFVSDISCRIM     = 7;
            const int DD_NEVSDISCRIM      = 9;
            const int DD_TQDCVSLIQTOF     = 11;
            const int DD_TQDCVSENERGY     = 13;
        }
    }
}

LiquidProcessor::LiquidProcessor() : EventProcessor(OFFSET, RANGE)
{
    name = "Liquid";
    associatedTypes.insert("scint");
}

void LiquidProcessor::DeclarePlots(void)
{
    //To handle Liquid Scintillators
    DeclareHistogram2D(DD_TQDCLIQUID, SC, S3, "Liquid vs. Trace QDC");
    DeclareHistogram2D(DD_MAXLIQUID, SC, S3, "Liquid vs. Maximum");
    DeclareHistogram2D(DD_DISCRIM, SA, S3, "N-Gamma Discrimination");
    DeclareHistogram2D(DD_TOFLIQUID, SE, S3,"Liquid vs. TOF");
    DeclareHistogram2D(DD_TRCLIQUID, S7, S7, "LIQUID TRACES");
    
    for(unsigned int i=0; i < 2; i++) { 
    	DeclareHistogram2D(DD_TQDCVSDISCRIM+i, SA, SE,"Trace QDC vs. NG Discrim");
    	DeclareHistogram2D(DD_TOFVSDISCRIM+i, SA, SA, "TOF vs. Discrim");
    	DeclareHistogram2D(DD_NEVSDISCRIM+i, SA, SE, "Energy vs. Discrim");
    	DeclareHistogram2D(DD_TQDCVSLIQTOF+i, SC, SE, "Trace QDC vs. Liquid TOF");
    	DeclareHistogram2D(DD_TQDCVSENERGY+i, SD, SE, "Trace QDC vs. Energy");
    }
}

bool LiquidProcessor::PreProcess(RawEvent &event){
    static const vector<ChanEvent*> &liquidEvents = 
	event.GetSummary("scint:liquid")->GetList();
    
    for (vector<ChanEvent*>::const_iterator it = liquidEvents.begin();
	 it != liquidEvents.end(); it++) {
        string place = (*it)->GetChanID().GetPlaceName();
        if (TreeCorrelator::get().places.count(place) == 1) {
            double time   = (*it)->GetTime();
            double energy = (*it)->GetCalEnergy();
            CorrEventData data(time, true, energy);
            TreeCorrelator::get().places[place]->activate(data);
        } else {
            cerr << "In LiquidProcessor: place " << place
                 << " does not exist." << endl;
            return false;
        }
    }
    return true;
}

bool LiquidProcessor::Process(RawEvent &event) {
    if (!EventProcessor::Process(event))
        return false;
    
    static const vector<ChanEvent*> &liquidEvents = 
	event.GetSummary("scint:liquid")->GetList();
    static const vector<ChanEvent*> &betaStartEvents = 
	event.GetSummary("scint:beta:start")->GetList();
    static const vector<ChanEvent*> &liquidStartEvents = 
	event.GetSummary("scint:liquid:start")->GetList();
    
    vector<ChanEvent*> startEvents;
    startEvents.insert(startEvents.end(), betaStartEvents.begin(),
		       betaStartEvents.end());
    startEvents.insert(startEvents.end(), liquidStartEvents.begin(),
		       liquidStartEvents.end());
    
    static int counter = 0;
    for(vector<ChanEvent*>::const_iterator itLiquid = liquidEvents.begin();
	itLiquid != liquidEvents.end(); itLiquid++) {
        unsigned int loc = (*itLiquid)->GetChanID().GetLocation();
        TimingInformation::TimingData liquid((*itLiquid));

        //Graph traces for the Liquid Scintillators
        if(liquid.discrimination == 0) {
            for(Trace::const_iterator i = liquid.trace.begin(); 
                i != liquid.trace.end(); i++)
                plot(DD_TRCLIQUID, int(i-liquid.trace.begin()), 
                     counter, int(*i)-liquid.aveBaseline);
            counter++;
        }
        
        if(liquid.dataValid) {
            plot(DD_TQDCLIQUID, liquid.tqdc, loc);
            plot(DD_MAXLIQUID, liquid.maxval, loc);
            
            double discrimNorm = 
                liquid.discrimination/liquid.tqdc;	    
            
            double discRes = 1000;
            double discOffset = 100;
            
            TimingInformation::TimingCal calibration =
                TimingInformation::GetTimingCal(make_pair(loc, "liquid"));
            
            if(discrimNorm > 0)
                plot(DD_DISCRIM, discrimNorm*discRes+discOffset, loc);
            plot(DD_TQDCVSDISCRIM, discrimNorm*discRes+discOffset,
                 liquid.tqdc);
            
            if((*itLiquid)->GetChanID().HasTag("start"))
                continue;
            
            for(vector<ChanEvent*>::iterator itStart = startEvents.begin(); 
                itStart != startEvents.end(); itStart++) { 
                unsigned int startLoc = (*itStart)->GetChanID().GetLocation();
                TimingInformation::TimingData start((*itStart));
                int histLoc = loc + startLoc;
                const int resMult = 2;
                const int resOffset = 2000;
                
                if(start.dataValid) {
                    double tofOffset;
                    if(startLoc == 0)
                        tofOffset = calibration.tofOffset0;
                    else
                        tofOffset = calibration.tofOffset1;
                    
                    double TOF = liquid.highResTime - 
                        start.highResTime - tofOffset; //in ns
                    double nEnergy = timeInfo.CalcEnergy(TOF, calibration.r0);
                    
                    plot(DD_TOFLIQUID, TOF*resMult+resOffset, histLoc);
                    plot(DD_TOFVSDISCRIM+histLoc, 
                         discrimNorm*discRes+discOffset, TOF*resMult+resOffset);
                    plot(DD_NEVSDISCRIM+histLoc, discrimNorm*discRes+discOffset, nEnergy);
                    plot(DD_TQDCVSLIQTOF+histLoc, TOF*resMult+resOffset, 
                         liquid.tqdc);
                    plot(DD_TQDCVSENERGY+histLoc, nEnergy, liquid.tqdc);
                }
            } //Loop over starts
        } // Good Liquid Check
    }//end loop over liquid events
    EndProcess();
    return true;
}
