//
//  EpanetModel.cpp
//  epanet-rtx
//
//  Created by the EPANET-RTX Development Team
//  See README.md and license.txt for more information
//  

#include <iostream>
#include "EpanetModel.h"
#include "rtxMacros.h"
#include "CurveFunction.h"

#include <types.h>

#include <boost/range/adaptors.hpp>
#include <boost/filesystem.hpp>

using namespace RTX;
using namespace std;

EpanetModel::EpanetModel() : Model() {
  // nothing to do, right?
  _enOpened = false;
  _enModel = NULL;
//  EN_API_CHECK( EN_newModel(&_enModel), "EN_newModel");
}

EpanetModel::~EpanetModel() {
  this->closeEngine();
  EN_API_CHECK( EN_close(_enModel), "EN_close");
  //  EN_API_CHECK(EN_freeModel(_enModel), "EN_freeModel");
}

EpanetModel::EpanetModel(const EpanetModel& o) {
  
  cout << "copy ctor : EpanetModel" << endl;
  
  this->useEpanetFile(o._modelFile);
  this->createRtxWrappers();
  
}

EN_Project* EpanetModel::epanetModelPointer() {
  return _enModel;
}

EpanetModel::EpanetModel(const std::string& filename) {
  try {
    this->useEpanetFile(filename);
    this->createRtxWrappers();
  }
  catch(const std::string& errStr) {
    std::cerr << "ERROR: ";
    throw RtxException("File Loading Error: " + errStr);
  }
}

#pragma mark - Loading

void EpanetModel::useEpanetFile(const std::string& filename) {
  
  Units volumeUnits(0);
  
  try {
    // set up temp path for report file so EPANET does not mess with stdout buffer
    boost::filesystem::path rptPath = boost::filesystem::temp_directory_path();
    rptPath /= "en_report.txt";
    
    EN_API_CHECK( EN_open((char*)filename.c_str(), &_enModel, (char*)rptPath.c_str(), (char*)""), "EN_open" );
    
  } catch (const std::string& errStr) {
    cerr << "model not formatted correctly. " << errStr << endl;
    throw "model not formatted correctly. " + errStr;
  }
  _modelFile = filename;
  
  // get units from epanet
  int flowUnitType = 0;
  bool isSI = false;
  
  int qualCode, traceNode;
  char chemName[32],chemUnits[32];
  EN_API_CHECK( EN_getqualinfo(_enModel, &qualCode, chemName, chemUnits, &traceNode), "EN_getqualinfo" );
  string chemUnitsStr(chemUnits);
  if (chemUnitsStr == "hrs") {
    chemUnitsStr = "hr";
  }
  Units qualUnits = Units::unitOfType(chemUnitsStr);
  this->setQualityUnits(qualUnits);
  
  EN_API_CHECK( EN_getflowunits(_enModel, &flowUnitType), "EN_getflowunits");
  switch (flowUnitType)
  {
    case EN_LPS:
    case EN_LPM:
    case EN_MLD:
    case EN_CMH:
    case EN_CMD:
      isSI = true;
      break;
    default:
      isSI = false;
  }
  switch (flowUnitType) {
    case EN_LPS:
      setFlowUnits(RTX_LITER_PER_SECOND);
      break;
    case EN_MLD:
      setFlowUnits(RTX_MILLION_LITER_PER_DAY);
      break;
    case EN_CMH:
      setFlowUnits(RTX_CUBIC_METER_PER_HOUR);
      break;
    case EN_CMD:
      setFlowUnits(RTX_CUBIC_METER_PER_DAY);
      break;
    case EN_GPM:
      setFlowUnits(RTX_GALLON_PER_MINUTE);
      break;
    case EN_MGD:
      setFlowUnits(RTX_MILLION_GALLON_PER_DAY);
      break;
    case EN_IMGD:
      setFlowUnits(RTX_IMPERIAL_MILLION_GALLON_PER_DAY);
      break;
    case EN_AFD:
      setFlowUnits(RTX_ACRE_FOOT_PER_DAY);
    default:
      break;
  }
  
  if (isSI) {
    setHeadUnits(RTX_METER);
    setPressureUnits(RTX_KILOPASCAL);
    volumeUnits = RTX_CUBIC_METER;
    
  }
  else {
    setHeadUnits(RTX_FOOT);
    setPressureUnits(RTX_PSI);
    volumeUnits = RTX_CUBIC_FOOT;
  }
  
  this->setVolumeUnits(volumeUnits);
  
  // what units are quality in? who knows!
  //this->setQualityUnits(RTX_MICROSIEMENS_PER_CM);
  //EN_API_CHECK(EN_setqualtype(_enModel, CHEM, (char*)"rtxConductivity", (char*)"us/cm", (char*)""), "EN_setqualtype");
  
  // get simulation parameters
  {
    long enTimeStep;
    EN_API_CHECK(EN_gettimeparam(_enModel, EN_HYDSTEP, &enTimeStep), "EN_gettimeparam EN_HYDSTEP");
    this->setHydraulicTimeStep((int)enTimeStep);
  }
  
  int nodeCount, tankCount, linkCount;
  char enName[RTX_MAX_CHAR_STRING];
  
  EN_API_CHECK( EN_getcount(_enModel, EN_NODECOUNT, &nodeCount), "EN_getcount EN_NODECOUNT" );
  EN_API_CHECK( EN_getcount(_enModel, EN_TANKCOUNT, &tankCount), "EN_getcount EN_TANKCOUNT" );
  EN_API_CHECK( EN_getcount(_enModel, EN_LINKCOUNT, &linkCount), "EN_getcount EN_LINKCOUNT" );
  
  // create lookup maps for name->index
  for (int iNode=1; iNode <= nodeCount; iNode++) {
    EN_API_CHECK( EN_getnodeid(_enModel, iNode, enName), "EN_getnodeid" );
    // and keep track of the epanet-toolkit index of this element
    _nodeIndex[string(enName)] = iNode;
  }
  
  for (int iLink = 1; iLink <= linkCount; iLink++) {
    EN_API_CHECK(EN_getlinkid(_enModel, iLink, enName), "EN_getlinkid");
    // keep track of this element index
    _linkIndex[string(enName)] = iLink;
  }
  
  
  // get the valve types
  for(Valve::_sp v : this->valves()) {
    int enIdx = _linkIndex[v->name()];
    EN_LinkType type = EN_PIPE;
    EN_getlinktype(_enModel, enIdx, &type);
    if (type == EN_PIPE) {
      // should not happen
      continue;
    }
    v->valveType = (int)type;
  }
  
  
  // copy my comments into the model
  for(Node::_sp n : this->nodes()) {
    this->setComment(n, n->userDescription());
  }
  for(Link::_sp l : this->links()) {
    this->setComment(l, l->userDescription());
  }
  
}

void EpanetModel::initEngine() {
  if (_enOpened) {
    EN_API_CHECK(EN_initH(_enModel, 10), "EN_initH");
    EN_API_CHECK(EN_initQ(_enModel, EN_NOSAVE), "EN_initQ");
    return;
  }
  try {
    EN_API_CHECK(EN_openH(_enModel), "EN_openH");
    EN_API_CHECK(EN_initH(_enModel, 10), "EN_initH");
    EN_API_CHECK(EN_openQ(_enModel), "EN_openQ");
    EN_API_CHECK(EN_initQ(_enModel, EN_NOSAVE), "EN_initQ");
  } catch (...) {
    cerr << "warning: epanet opened improperly" << endl;
  }
  _enOpened = true;
  this->applyInitialQuality();
}

void EpanetModel::closeEngine() {
  if (_enOpened) {
    try {
      EN_API_CHECK( EN_closeQ(_enModel), "EN_closeQ");
      EN_API_CHECK( EN_closeH(_enModel), "EN_closeH");
    } catch (...) {
      cerr << "warning: epanet closed improperly" << endl;
    }
    _enOpened = false;
  }
  
}


void EpanetModel::createRtxWrappers() {
  
  int curveCount, nodeCount, tankCount, linkCount;
  
  try {
    EN_API_CHECK( EN_getcount(_enModel, EN_CURVECOUNT, &curveCount), "EN_getcount EN_CURVECOUNT");
    EN_API_CHECK( EN_getcount(_enModel, EN_NODECOUNT, &nodeCount), "EN_getcount EN_NODECOUNT" );
    EN_API_CHECK( EN_getcount(_enModel, EN_TANKCOUNT, &tankCount), "EN_getcount EN_TANKCOUNT" );
    EN_API_CHECK( EN_getcount(_enModel, EN_LINKCOUNT, &linkCount), "EN_getcount EN_LINKCOUNT" );
  } catch (...) {
    throw "Could not create wrappers";
  }
  
  map<int,Curve::_sp> namedCurves;
  
  for (int iCurve = 1; iCurve <= curveCount; ++iCurve) {
    double *xVals, *yVals;
    int nPoints;
    char buf[1024];
    int ok = EN_getcurve (_enModel, iCurve, buf, &nPoints, &xVals, &yVals);
    
    map<double,double> curveData;
    for (int iPoint = 0; iPoint < nPoints; ++iPoint) {
      curveData[xVals[iPoint]] = yVals[iPoint];
    }
    
    Curve::_sp newCurve( new Curve );
    newCurve->curveData = curveData;
    newCurve->inputUnits = RTX_DIMENSIONLESS;
    newCurve->outputUnits = RTX_DIMENSIONLESS;
    newCurve->name = string(buf);
    
    this->addCurve(newCurve);
    namedCurves[iCurve] = newCurve;
  }
  
  
  
  // create nodes
  for (int iNode=1; iNode <= nodeCount; iNode++) {
    char enName[RTX_MAX_CHAR_STRING];
    double x,y,z;         // rtx coordinates
    EN_NodeType nodeType;         // epanet node type code
    string nodeName, comment;
    Junction::_sp newJunction;
    Reservoir::_sp newReservoir;
    Tank::_sp newTank;
    char enComment[MAXMSG];
    
    // get relevant info from EPANET toolkit
    EN_API_CHECK( EN_getnodeid(_enModel, iNode, enName), "EN_getnodeid" );
    EN_API_CHECK( EN_getnodevalue(_enModel, iNode, EN_ELEVATION, &z), "EN_getnodevalue EN_ELEVATION");
    EN_API_CHECK( EN_getnodetype(_enModel, iNode, &nodeType), "EN_getnodetype");
    EN_API_CHECK( EN_getcoord(_enModel, iNode, &x, &y), "EN_getcoord");
    EN_API_CHECK( EN_getnodecomment(_enModel, iNode, enComment), "EN_getnodecomment");
    
    nodeName = string(enName);
    comment = string(enComment);
    double minLevel = 0, maxLevel = 0;
    
    switch (nodeType) {
      case EN_TANK:
      {
        newTank.reset( new Tank(nodeName) );
        // get tank geometry from epanet and pass it along
        // todo -- geometry
        
        addTank(newTank);
        
        EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MAXLEVEL, &maxLevel), "EN_getnodevalue(EN_MAXLEVEL)");
        EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MINLEVEL, &minLevel), "EN_getnodevalue(EN_MINLEVEL)");
        newTank->setMinMaxLevel(minLevel, maxLevel);
        
        newTank->level()->setUnits(headUnits());
        newTank->flowCalc()->setUnits(flowUnits());
        newTank->volumeCalc()->setUnits(volumeUnits());
        newTank->flow()->setUnits(flowUnits());
        newTank->volume()->setUnits(volumeUnits());
        
        Curve::_sp volumeCurve;
        double volumeCurveIndex;
        EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_VOLCURVE, &volumeCurveIndex), "EN_getnodevalue EN_VOLCURVE");
        
        if (volumeCurveIndex > 0) {
          // curved tank
          volumeCurve = namedCurves[volumeCurveIndex];
        }
        else {
          // it's a cylindrical tank - invent a curve
          double minVolume, maxVolume;
          double minLevel, maxLevel;
          
          EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MAXLEVEL, &maxLevel), "EN_MAXLEVEL");
          EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MINLEVEL, &minLevel), "EN_MINLEVEL");
          EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MINVOLUME, &minVolume), "EN_MINVOLUME");
          EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_MAXVOLUME, &maxVolume), "EN_MAXVOLUME");
          
          volumeCurve.reset( new Curve );
          volumeCurve->curveData[minLevel] = minVolume;
          volumeCurve->curveData[maxLevel] = maxVolume;
          
          stringstream ss;
          ss << "Tank " << newTank->name() << " Cylindrical Curve";
          volumeCurve->name = ss.str();
          this->addCurve(volumeCurve); // unnamed curve: add cylindrical curve to list
        }
        
        volumeCurve->inputUnits = this->headUnits();
        volumeCurve->outputUnits = this->volumeUnits();
        
        // set tank geometry
        newTank->setGeometry(volumeCurve);
        
        newJunction = newTank;
        break;
      }
      case EN_RESERVOIR:
        newReservoir.reset( new Reservoir(nodeName) );
        addReservoir(newReservoir);
        newJunction = newReservoir;
        break;
      case EN_JUNCTION:
        newJunction.reset( new Junction(nodeName) );
        addJunction(newJunction);
        break;
      default:
        throw "Node Type Unknown";
    } // switch nodeType
    
    // set units for new element
    newJunction->head()->setUnits(headUnits());
    newJunction->pressure()->setUnits(pressureUnits());
    newJunction->demand()->setUnits(flowUnits());
    newJunction->quality()->setUnits(qualityUnits());
    
    // newJunction is the generic (base-class) pointer to the specific object,
    // so we can use base-class methods to set some parameters.
    newJunction->setElevation(z);
    newJunction->setCoordinates(Node::location_t(x, y));
    
    // Initial quality specified in input data
    double initQual;
    EN_API_CHECK(EN_getnodevalue(_enModel, iNode, EN_INITQUAL, &initQual), "EN_INITQUAL");
    newJunction->state_quality = initQual;
    
    // Base demand is sum of all demand categories, accounting for patterns
    double demand = 0, categoryDemand = 0, avgPatternValue = 0;
    int numDemands = 0, patternIdx = 0;
    EN_API_CHECK( EN_getnumdemands(_enModel, iNode, &numDemands), "EN_getnumdemands()");
    for (int demandIdx = 1; demandIdx <= numDemands; demandIdx++) {
      EN_API_CHECK( EN_getbasedemand(_enModel, iNode, demandIdx, &categoryDemand), "EN_getbasedemand()" );
      EN_API_CHECK( EN_getdemandpattern(_enModel, iNode, demandIdx, &patternIdx), "EN_getdemandpattern()");
      avgPatternValue = 1.0;
      if (patternIdx > 0) { // Not the default "pattern" = 1
        EN_API_CHECK( EN_getaveragepatternvalue(_enModel, patternIdx, &avgPatternValue), "EN_getaveragepatternvalue()");
      }
      demand += categoryDemand * avgPatternValue;
    }
    newJunction->setBaseDemand(demand);
    newJunction->setUserDescription(comment);
    
    
  } // for iNode
  
  // create links
  for (int iLink = 1; iLink <= linkCount; iLink++) {
    char enLinkName[RTX_MAX_CHAR_STRING], enFromName[RTX_MAX_CHAR_STRING], enToName[RTX_MAX_CHAR_STRING], enComment[RTX_MAX_CHAR_STRING];
    int enFrom, enTo;
    EN_LinkType linkType;
    double length, diameter, status, rough, mloss, setting, curveIdx;
    string linkName, comment;
    Node::_sp startNode, endNode;
    Pipe::_sp newPipe;
    Pump::_sp newPump;
    Valve::_sp newValve;
    
    // a bunch of epanet api calls to get properties from the link
    EN_API_CHECK(EN_getlinkid(_enModel, iLink, enLinkName), "EN_getlinkid");
    EN_API_CHECK(EN_getlinktype(_enModel, iLink, &linkType), "EN_getlinktype");
    EN_API_CHECK(EN_getlinknodes(_enModel, iLink, &enFrom, &enTo), "EN_getlinknodes");
    EN_API_CHECK(EN_getnodeid(_enModel, enFrom, enFromName), "EN_getnodeid - enFromName");
    EN_API_CHECK(EN_getnodeid(_enModel, enTo, enToName), "EN_getnodeid - enToName");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_DIAMETER, &diameter), "EN_getlinkvalue EN_DIAMETER");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_LENGTH, &length), "EN_getlinkvalue EN_LENGTH");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_INITSTATUS, &status), "EN_getlinkvalue EN_STATUS");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_ROUGHNESS, &rough), "EN_getlinkvalue EN_ROUGHNESS");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_MINORLOSS, &mloss), "EN_getlinkvalue EN_MINORLOSS");
    EN_API_CHECK(EN_getlinkvalue(_enModel, iLink, EN_INITSETTING, &setting), "EN_getlinkvalue EN_INITSETTING");
    EN_API_CHECK(EN_getlinkcomment(_enModel, iLink, enComment), "EN_getlinkcomment");
    
    linkName = string(enLinkName);
    comment = string(enComment);
    // get node pointers
    startNode = nodeWithName(string(enFromName));
    endNode = nodeWithName(string(enToName));
    
    if (! (startNode && endNode) ) {
      std::cerr << "could not find nodes for link " << linkName << std::endl;
      throw "nodes not found";
    }
    
    
    // create the new specific type and add it.
    // newPipe becomes the generic (base-class) pointer in all cases.
    switch (linkType) {
      case EN_PIPE:
        newPipe.reset( new Pipe(linkName) );
        newPipe->setNodes(startNode, endNode);
        addPipe(newPipe);
        break;
      case EN_PUMP:
        newPump.reset( new Pump(linkName) );
        newPump->setNodes(startNode, endNode);
        newPipe = newPump;
        addPump(newPump);
        
      {
        // has curve?
        int err = EN_getlinkvalue(_enModel, iLink, EN_HEADCURVE, &curveIdx);
        if (err == EN_OK) {
          Curve::_sp pumpCurve = namedCurves[(int)curveIdx];
          if (pumpCurve) {
            pumpCurve->inputUnits = this->flowUnits();
            pumpCurve->outputUnits = this->headUnits();
            newPump->setHeadCurve(pumpCurve);
          }
        }
        err = EN_getlinkvalue(_enModel, iLink, EN_EFFICIENCYCURVE, &curveIdx);
        if (err == EN_OK) {
          Curve::_sp effCurve = namedCurves[(int)curveIdx];
          if (effCurve) {
            effCurve->inputUnits = this->flowUnits();
            effCurve->outputUnits = RTX_DIMENSIONLESS;
            newPump->setEfficiencyCurve(effCurve);
          }
        }
      }
        break;
      case EN_CVPIPE:
      case EN_PSV:
      case EN_PRV:
      case EN_FCV:
      case EN_PBV:
      case EN_TCV:
      case EN_GPV:
        newValve.reset( new Valve(linkName) );
        newValve->setNodes(startNode, endNode);
        newValve->valveType = (int)linkType;
        newValve->fixedSetting = setting;
        newPipe = newValve;
        addValve(newValve);
        break;
      default:
        std::cerr << "could not find pipe type" << std::endl;
        break;
    } // switch linkType
    
    
    // now that the pipe is created, set some basic properties.
    newPipe->setDiameter(diameter);
    newPipe->setLength(length);
    newPipe->setRoughness(rough);
    newPipe->setMinorLoss(mloss);
    
    if (status == 0) {
      newPipe->setFixedStatus(Pipe::CLOSED);
    }
    
    newPipe->flow()->setUnits(flowUnits());
    newPipe->setUserDescription(comment);
    
    
  } // for iLink
  
}

void EpanetModel::overrideControls() throw(RTX::RtxException) {
  // set up counting variables for creating model elements.
  int nodeCount, tankCount;
  
  try {
    EN_API_CHECK( EN_getcount(_enModel, EN_NODECOUNT, &nodeCount), "EN_getcount EN_NODECOUNT" );
    EN_API_CHECK( EN_getcount(_enModel, EN_TANKCOUNT, &tankCount), "EN_getcount EN_TANKCOUNT" );
    
    // eliminate all patterns and control rules
    
    int nPatterns;
    double sourcePat;
    double zeroPattern[2];
    zeroPattern[0] = 0;
    
    // clear all patterns, set to zero.  we do this to get rid of extra demand patterns set in [DEMANDS],
    // since there's no other way to get to them using the toolkit.
    EN_API_CHECK( EN_getcount(_enModel, EN_PATCOUNT, &nPatterns), "EN_getcount(EN_PATCOUNT)" );
    for (int i=1; i<=nPatterns; i++) {
      EN_API_CHECK( EN_setpattern(_enModel, i, zeroPattern, 1), "EN_setpattern" );
    }
    for(int iNode = 1; iNode <= nodeCount ; iNode++)  {
      // set pattern to index 0 (unity multiplier). this also rids us of any multiple demand patterns.
      EN_API_CHECK( EN_setnodevalue(_enModel, iNode, EN_PATTERN, 0 ), "EN_setnodevalue(EN_PATTERN)" );  // demand patterns to unity
      EN_API_CHECK( EN_setnodevalue(_enModel, iNode, EN_BASEDEMAND, 0. ), "EN_setnodevalue(EN_BASEDEMAND)" );	// set base demand to zero
      // look for a quality source and nullify its existance
      int errCode = EN_getnodevalue(_enModel, iNode, EN_SOURCEPAT, &sourcePat);
      if (errCode != EN_ERR_UNDEF_SOURCE) {
        EN_API_CHECK( EN_setnodevalue(_enModel, iNode, EN_SOURCETYPE, EN_CONCEN), "EN_setnodevalue(EN_SOURCETYPE)" );
        EN_API_CHECK( EN_setnodevalue(_enModel, iNode, EN_SOURCEQUAL, 0.), "EN_setnodevalue(EN_SOURCEQUAL)" );
        EN_API_CHECK( EN_setnodevalue(_enModel, iNode, EN_SOURCEPAT, 0.), "EN_setnodevalue(EN_SOURCEPAT)" );
      }
    }
    // set the global demand multiplier is unity as well.
    EN_setoption(_enModel, EN_DEMANDMULT, 1.);
    
    // disregard controls and rules.
    this->disableControls();
  }
  catch(string error) {
    std::cerr << "ERROR: " << error;
    throw RtxException();
  }
  // base class implementation
  Model::overrideControls();
}

#pragma mark - Protected Methods:

std::ostream& EpanetModel::toStream(std::ostream &stream) {
  // epanet-specific printing
  stream << "Epanet Model File: " << endl;
  Model::toStream(stream);
  return stream;
}

#pragma mark Setters

/* setting simulation parameters */

void EpanetModel::setReservoirHead(const string& reservoir, double level) {
  setNodeValue(EN_TANKLEVEL, reservoir, level);
}

void EpanetModel::setReservoirQuality(const string& reservoir, double quality) {
  setNodeValue(EN_INITQUAL, reservoir, quality); // set initquality in case setpoint is lower than old value
  setNodeValue(EN_SOURCETYPE, reservoir, CONCEN);
  setNodeValue(EN_SOURCEQUAL, reservoir, quality);
}

void EpanetModel::setTankLevel(const string& tank, double level) {
  // just refer to the reservoir method, since in epanet they are the same thing.
  setReservoirHead(tank, level);
}

void EpanetModel::setJunctionDemand(const string& junction, double demand) {
  int nodeIndex = _nodeIndex[junction];
  // Junction demand is total demand - so deal with multiple categories
  int numDemands = 0;
  EN_API_CHECK( EN_getnumdemands(_enModel, nodeIndex, &numDemands), "EN_getnumdemands()");
  for (int demandIdx = 1; demandIdx < numDemands; demandIdx++) {
    EN_API_CHECK( EN_setbasedemand(_enModel, nodeIndex, demandIdx, 0.0), "EN_setbasedemand()" );
  }
  // Last demand category is the one... per EPANET convention
  EN_API_CHECK( EN_setbasedemand(_enModel, nodeIndex, numDemands, demand), "EN_setbasedemand()" );
}

void EpanetModel::setJunctionQuality(const std::string& junction, double quality) {
  // todo - add more source types, depending on time series dimension?
  setNodeValue(EN_INITQUAL, junction, quality); // set initquality in case setpoint is lower than old value
  setNodeValue(EN_SOURCETYPE, junction, FLOWPACED);
  setNodeValue(EN_SOURCEQUAL, junction, quality);
}

void EpanetModel::setPipeStatus(const string& pipe, Pipe::status_t status) {
  setLinkValue(EN_STATUS, pipe, status);
}

void EpanetModel::setPumpStatus(const string& pump, Pipe::status_t status) {
  // call the setPipeStatus method, since they are the same in epanet.
  setPipeStatus(pump, status);
}

void EpanetModel::setPumpSetting(const string& pump, double setting) {
  setLinkValue(EN_SETTING, pump, setting);
}

void EpanetModel::setValveSetting(const string& valve, double setting) {
  setLinkValue(EN_SETTING, valve, setting);
}

#pragma mark Getters

double EpanetModel::junctionDemand(const string &junction) {
  return getNodeValue(EN_DEMAND, junction);
}

double EpanetModel::junctionHead(const string &junction) {
  return getNodeValue(EN_HEAD, junction);
}
double EpanetModel::junctionPressure(const string &junction) {
  return getNodeValue(EN_PRESSURE, junction);
}

double EpanetModel::junctionQuality(const string &junction) {
  return getNodeValue(EN_QUALITY, junction);
}

double EpanetModel::reservoirLevel(const string &reservoir) {
  return getNodeValue(EN_TANKLEVEL, reservoir);
}

double EpanetModel::tankLevel(const string &tank) {
  return junctionHead(tank) - nodeWithName(tank)->elevation(); // node elevation & head in same Epanet units
}

double EpanetModel::tankVolume(const string& tank) {
  return getNodeValue(EN_TANKVOLUME, tank);
}

double EpanetModel::tankFlow(const string& tank) {
  return getNodeValue(EN_DEMAND, tank);
}

double EpanetModel::pipeFlow(const string &pipe) {
  return getLinkValue(EN_FLOW, pipe);
}
double EpanetModel::pipeStatus(const string &pipe) {
  return getLinkValue(EN_STATUS, pipe);
}
double EpanetModel::pipeSetting(const string &pipe) {
  return getLinkValue(EN_SETTING, pipe);
}

double EpanetModel::pumpEnergy(const string &pump) {
  return getLinkValue(EN_ENERGY, pump);
}

#pragma mark - Sim options
void EpanetModel::enableControls() {
  int nC;
  EN_getcount(_enModel, EN_CONTROLCOUNT, &nC);
  
  for (int i = 1; i <= nC; ++i) {
    EN_setControlEnabled(_enModel, i, EN_ENABLE);
  }
  EN_getcount(_enModel, EN_RULECOUNT, &nC);
  for (int i = 1; i <= nC; ++i) {
    EN_setRuleEnabled(_enModel, i, EN_ENABLE);
  }
}

void EpanetModel::disableControls() {
  int nC;
  EN_getcount(_enModel, EN_CONTROLCOUNT, &nC);
  
  for (int i = 1; i <= nC; ++i) {
    EN_setControlEnabled(_enModel, i, EN_DISABLE);
  }
  
  EN_getcount(_enModel, EN_RULECOUNT, &nC);
  for (int i = 1; i <= nC; ++i) {
    EN_setRuleEnabled(_enModel, i, EN_DISABLE);
  }
}



#pragma mark Simulation Methods

bool EpanetModel::solveSimulation(time_t time) {
  // TODO -- 
  /*
      determine how to perform this function. maybe keep track of relative error for each simulation period
   so we know what simulation periods have been solved - as in a master simulation clock?
   
   */
  bool success = true;
  long timestep;
  int errorCode;
  
  // set the current epanet-time to zero, since we override epanet-time.
  setCurrentSimulationTime( time );
  EN_API_CHECK(EN_settimeparam(_enModel, EN_HTIME, 0), "EN_settimeparam(EN_HTIME)");
  EN_API_CHECK(EN_settimeparam(_enModel, EN_QTIME, 0), "EN_settimeparam(EN_QTIME)");
  // solve the hydraulics
  EN_API_CHECK(errorCode = EN_runH(_enModel, &timestep), "EN_runH");
  // check for success
  success = this->_didConverge(time, errorCode);
  
  if (errorCode > 0) {
    char errorMsg[256];
    EN_geterror(errorCode, errorMsg, 255);
    struct tm * timeinfo = localtime (&time);
    stringstream ss;
    ss << std::string(errorMsg) << " :: " << asctime(timeinfo);
    this->logLine(ss.str());
  }

  // how to deal with lack of hydraulic convergence here - reset boundary/initial conditions?
  if (this->shouldRunWaterQuality()) {
    EN_API_CHECK(EN_runQ(_enModel, &timestep), "EN_runQ");
  }
  
  return success;
}

time_t EpanetModel::nextHydraulicStep(time_t time) {
  if ( time != currentSimulationTime() ) {
    // todo - throw something?
    cerr << "time not synchronized!" << endl;
  }
  // get the time of the next hydraulic event (according to the simulation)
  time_t nextTime = time;
  // re-set the epanet engine's hydstep parameter to the original value,
  // so that the step length figurer-outerer works.
  int actualTimeStep = hydraulicTimeStep();
  this->setHydraulicTimeStep(actualTimeStep);
  
  // get time to next hydraulic event
  EN_TimestepEvent eventType;
  long duration = 0;
  int elementIndex = 0;
  EN_API_CHECK(EN_timeToNextEvent(_enModel, &eventType, &duration, &elementIndex), "EN_timeToNextEvent");
  nextTime += duration;
  
  if (eventType == EN_STEP_TANKEVENT || eventType == EN_STEP_CONTROLEVENT) {
    
    string elementTypeStr("");
    string elementDescStr("");
    
    if (eventType == EN_STEP_TANKEVENT) {
      elementTypeStr = "Tank";
      char id[MAXID+1];
      EN_API_CHECK( EN_getnodeid(_enModel, elementIndex, id), "EN_getnodeid()" );
      elementDescStr = string(id);
    }
    else {
      elementTypeStr = "Control";
      elementDescStr = "index " + to_string(elementIndex) + " :: ";
      
      int controlType, linkIndex, nodeIndex;
      double setting, level;
      EN_getcontrol(_enModel, elementIndex, &controlType, &linkIndex, &setting, &nodeIndex, &level);
      //
      //#define EN_LOWLEVEL     0   /* Control types.  */
      //#define EN_HILEVEL      1   /* See ControlType */
      //#define EN_TIMER        2   /* in TYPES.H.     */
      //#define EN_TIMEOFDAY    3
      
      char linkName[1024];
      EN_getlinkid(_enModel, linkIndex, linkName);
      
      string nodeOrTime("");
      if (nodeIndex == 0) {
        nodeOrTime = "Time";
      }
      else {
        char nodeId[1024];
        EN_getnodeid(_enModel, nodeIndex, nodeId);
        nodeOrTime = string(nodeId);
      }
      
      stringstream controlString;
      switch (controlType) {
        case 0:
          controlString << "Low Level: "<< linkName << "= " << setting << " when " << nodeOrTime << " < " << level;
          break;
        case 1:
          controlString << "High Level: " << linkName << " = " << setting << " when " << nodeOrTime << " > " << level;
          break;
        case 2:
          controlString << "Timer: " << linkName << " = " << setting << " when " << nodeOrTime << " = " << level;
          break;
        case 3:
          controlString << "Time of Day: " << linkName << " = " << setting << " when " << nodeOrTime << " = " << level;
          break;
        default:
          break;
      }

      elementDescStr += controlString.str();
      
    }
    
    stringstream ss;
    ss << "INFO: Simulation step restricted to " << duration << " seconds by " << elementTypeStr << " :: " << elementDescStr;
    this->logLine(ss.str());
  }
  
  return nextTime;
}

// evolve tank levels
void EpanetModel::stepSimulation(time_t time) {
  int step = (int)(time - this->currentSimulationTime());
  long qstep = step;
  
  //std::cout << "set step to: " << step << std::endl;
  
  long computedStep = 0;
  
  int actualStep = this->hydraulicTimeStep();
  this->setHydraulicTimeStep(step);
  EN_API_CHECK( EN_nextH(_enModel, &computedStep), "EN_nextH()" );
  
  if (this->shouldRunWaterQuality()) {
    EN_API_CHECK(EN_nextQ(_enModel, &qstep), "EN_nextQ");
  }
  
  if (step != computedStep) {
    // it's an intermediate step
    stringstream ss;
    ss << "ERROR: Simulation step used for updating tank levels different than expected: set " << step << " and simulated " << computedStep;
    this->logLine(ss.str());
  }
  
  this->setHydraulicTimeStep(actualStep);
  setCurrentSimulationTime( currentSimulationTime() + step );
}

int EpanetModel::iterations(time_t time) {
  double iterations;
  EN_API_CHECK( EN_getstatistic(_enModel, EN_ITERATIONS, &iterations), "EN_getstatistic(EN_ITERATIONS)");
  return iterations;
}

double EpanetModel::relativeError(time_t time) {
  double relativeError;
  EN_API_CHECK( EN_getstatistic(_enModel, EN_RELATIVEERROR, &relativeError), "EN_getstatistic(EN_RELATIVEERROR)");
  return relativeError;
}

bool EpanetModel::_didConverge(time_t time, int errorCode) {
  // return true if the simulation did converge
  EN_API_FLOAT_TYPE accuracy;
  
  EN_API_CHECK( EN_getoption(_enModel, EN_ACCURACY, &accuracy), "EN_getoption");
  bool illcondition = errorCode == 101 || errorCode == 110; // 101 is memory issue, 110 is illconditioning
  bool unbalanced = relativeError(time) > accuracy;
  if (illcondition || unbalanced) {
    return false;
  }
  else {
    return true;
  }
}


void EpanetModel::setHydraulicTimeStep(int seconds) {
  EN_API_CHECK( EN_settimeparam(_enModel, EN_DURATION, (long)seconds), "EN_settimeparam(EN_DURATION)" );
//  EN_API_CHECK( EN_settimeparam(_enModel, EN_REPORTSTEP, (long)seconds), "EN_settimeparam(EN_REPORTSTEP)" );
  EN_API_CHECK( EN_settimeparam(_enModel, EN_HYDSTEP, (long)seconds), "EN_settimeparam(EN_HYDSTEP)" );
  // base class method
  Model::setHydraulicTimeStep(seconds);
  
}

void EpanetModel::setQualityTimeStep(int seconds) {
  EN_API_CHECK( EN_settimeparam(_enModel, EN_QUALSTEP, (long)seconds), "EN_settimeparam(EN_QUALSTEP)");
  // call base class
  Model::setQualityTimeStep(seconds);
}

void EpanetModel::applyInitialQuality() {
  if (!_enOpened) {
    DebugLog << "Could not apply initial quality conditions; engine not opened" << EOL;
    return;
  }
  EN_API_CHECK(EN_closeQ(_enModel), "EN_closeQ");
  EN_API_CHECK(EN_openQ(_enModel), "EN_openQ");

  // Junctions
  for(Junction::_sp junc : this->junctions()) {
    double qual = junc->state_quality;
    int iNode = _nodeIndex[junc->name()];
    EN_API_CHECK(EN_setnodevalue(_enModel, iNode, EN_INITQUAL, qual), "EN_setnodevalue - EN_INITQUAL");
  }
  
  // Tanks
  for(Tank::_sp tank : this->tanks()) {
    double qual = tank->state_quality;
    int iNode = _nodeIndex[tank->name()];
    EN_API_CHECK(EN_setnodevalue(_enModel, iNode, EN_INITQUAL, qual), "EN_setnodevalue - EN_INITQUAL");
  }
  
  // reservoirs
  for(auto r: this->reservoirs()) {
    double q = r->state_quality;
    int iNode = _nodeIndex[r->name()];
    EN_API_CHECK(EN_setnodevalue(_enModel, iNode, EN_INITQUAL, q), "EN_setnodevalue - EN_INITIALQUAL");
  }
  
  EN_API_CHECK(EN_initQ(_enModel, EN_NOSAVE), "ENinitQ");
}



void EpanetModel::updateEngineWithElementProperties(Element::_sp e) {
  
  //! update hydraulic engine representation with full list of properties from this element.
  
  switch (e->type()) {
    case Element::TANK:
    {
      Tank::_sp t = std::dynamic_pointer_cast<Tank>(e);
      this->setNodeValue(EN_MINLEVEL, t->name(), t->minLevel());
      this->setNodeValue(EN_MAXLEVEL, t->name(), t->maxLevel());
    }
    case Element::JUNCTION:
    case Element::RESERVOIR:
    {
      Junction::_sp j = std::dynamic_pointer_cast<Junction>(e);
      this->setNodeValue(EN_ELEVATION, j->name(), j->elevation());
      this->setNodeValue(EN_BASEDEMAND, j->name(), j->baseDemand());
      this->setComment(j, j->userDescription());
    }
      break;
      
    case Element::VALVE:
    case Element::PUMP:
    case Element::PIPE:
    {
      Pipe::_sp p = std::dynamic_pointer_cast<Pipe>(e);
      this->setLinkValue(EN_DIAMETER, p->name(), p->diameter());
      this->setLinkValue(EN_ROUGHNESS, p->name(), p->roughness());
      this->setLinkValue(EN_LENGTH, p->name(), p->length());
      // is it check valve? can't set status
      Valve::_sp v = std::dynamic_pointer_cast<Valve>(p);
      if (!(v && v->valveType == EN_CVPIPE)) {
        this->setLinkValue(EN_STATUS, p->name(), p->fixedStatus());
      }
      this->setComment(p, p->userDescription());
    }
      break;
    default:
      break;
  }
}

#pragma mark -
#pragma mark Internal Private Methods

double EpanetModel::getNodeValue(int epanetCode, const string& node) {
  int nodeIndex = _nodeIndex[node];
  double value;
  EN_API_CHECK(EN_getnodevalue(_enModel, nodeIndex, epanetCode, &value), "EN_getnodevalue");
  return value;
}
void EpanetModel::setNodeValue(int epanetCode, const string& node, double value) {
  int nodeIndex = _nodeIndex[node];
  stringstream s;
  s << "EN_setnodevalue " << node << " : " << value;
  const string str = s.str();
  EN_API_CHECK(EN_setnodevalue(_enModel, nodeIndex, epanetCode, value), str);
}

double EpanetModel::getLinkValue(int epanetCode, const string& link) {
  int linkIndex = _linkIndex[link];
  double value;
  EN_API_CHECK(EN_getlinkvalue(_enModel, linkIndex, epanetCode, &value), "EN_getlinkvalue");
  return value;
}
void EpanetModel::setLinkValue(int epanetCode, const string& link, double value) {
  int linkIndex = _linkIndex[link];
  EN_API_CHECK(EN_setlinkvalue(_enModel, linkIndex, epanetCode, value), "EN_setlinkvalue");
}

void EpanetModel::setComment(Element::_sp element, const std::string& comment)
{
  
  switch (element->type()) {
    case Element::JUNCTION:
    case Element::TANK:
    case Element::RESERVOIR:
    {
      int nodeIndex = _nodeIndex[element->name()];
      EN_API_CHECK(EN_setnodecomment(_enModel, nodeIndex, comment.c_str()), "EN_setnodecomment");
    }
      break;
    case Element::PIPE:
    case Element::PUMP:
    case Element::VALVE:
    {
      int linkIndex = _linkIndex[element->name()];
      EN_API_CHECK(EN_setlinkcomment(_enModel, linkIndex, comment.c_str()), "EN_setlinkcomment");
    }
      break;
    default:
      break;
  }
  
}

void EpanetModel::EN_API_CHECK(int errorCode, string externalFunction) throw(string) {
  if (errorCode > 10) {
    char errorMsg[256];
    EN_geterror(errorCode, errorMsg, 255);
    this->logLine(string(externalFunction + " :: " + errorMsg));
    throw externalFunction + "::" + std::string(errorMsg);
  }
}


int EpanetModel::enIndexForJunction(Junction::_sp j) {
  if (_nodeIndex.count(j->name()) > 0) {
    return _nodeIndex.at(j->name());
  }
  else {
    return -1;
  }
}

int EpanetModel::enIndexForPipe(Pipe::_sp p) {
  if (_linkIndex.count(p->name()) > 0) {
    return _linkIndex.at(p->name());
  }
  else {
    return -1;
  }
}

