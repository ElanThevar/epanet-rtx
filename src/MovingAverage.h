//
//  MovingAverage.h
//  epanet_rtx
//
//  Created by the EPANET-RTX Development Team
//  See README.md and license.txt for more information
//  

#ifndef rtx_movingaverage_h
#define rtx_movingaverage_h

#include "TimeSeriesFilter.h"

using std::vector;


/**
 a centered moving average filter, using the "source" timeseries as input.
 */

namespace RTX {
  class MovingAverage : public TimeSeriesFilter {
  public:
    RTX_BASE_PROPS(MovingAverage);
    MovingAverage();
    
    // class-specific properties
    void setWindowSize(int numberOfPoints);   /// set number of points to consider in the moving average calculation
    int windowSize();                         /// return the window size (see above)
    
    MovingAverage::_sp window(int nPoints) {this->setWindowSize(nPoints); return share_me(this);};
    
  protected:
    PointCollection filterPointsInRange(TimeRange range);
    
  private:
    int _windowSize;
  };
}


#endif
