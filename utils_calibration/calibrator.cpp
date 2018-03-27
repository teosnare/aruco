/**
Copyright 2017 Rafael Muñoz Salinas. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of
      conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, this list
      of conditions and the following disclaimer in the documentation and/or other materials
      provided with the distribution.

THIS SOFTWARE IS PROVIDED BY Rafael Muñoz Salinas ''AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Rafael Muñoz Salinas OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the
authors and should not be interpreted as representing official policies, either expressed
or implied, of Rafael Muñoz Salinas.
*/

#include "markermap.h"
#include "calibrator.h"
#include "levmarq.h"
#include <opencv2/calib3d.hpp>
using namespace std;
namespace aruco{
//portable calibration function



Calibrator::Calibrator(){
    strinfo="Not calibrated. Need at least 4 images";
    _mmap=getDefaultCalibrationBoard();

}
Calibrator::~Calibrator(){
    stopThread();
}

void Calibrator::setParams(cv::Size imageSize,float markerSize,std::string markerMap ){
    try{
        stopThread();
        _imageSize=imageSize;
        if (!markerMap.empty()) _mmap.readFromFile(markerMap);

        _msize=markerSize;
        startThread();
    }catch(std::exception &ex){throw ex;}
}
//add a detection of the calibration markerset and do calibration
void Calibrator::addView(const std::vector<aruco::Marker> &markers){

    if ( _mmap.getIndices(markers).size()>=3){//at least three markers of the markermap
        std::unique_lock<std::mutex> lock(markerstmpMutex);
        std::unique_lock<std::mutex>vmarkerslock(markersMutex);
        vmarkerstmp.push_back(markers);
        if (vmarkers.size()<4){
            int nneeded=4-(vmarkerstmp.size()+vmarkers.size());
            setInfoString("Not calibrated. Need at least "+to_string(nneeded)+" more images");
        }
    }
}

//returns a string with info about the status
std::string Calibrator::getInfo(){

        std::unique_lock<std::mutex> lock(infoMutex);
        std::string str=strinfo;
        return str;
}
void  Calibrator::setInfoString(const std::string &str){
     std::unique_lock<std::mutex> lock(infoMutex);
     strinfo=str;
}
//obtain the camera calibration results. It is blocking function
bool Calibrator::getCalibrationResults( aruco::CameraParameters &camp){
    bool isOk=true;

    aruco::CameraParameters ret;
    calibrationMutex.lock();
    if ( vmarkers.size()<3) isOk=false;
    camp=cameraParams;
    calibrationMutex.unlock();

    return isOk;
}

int Calibrator::getNumberOfViews(){
    std::unique_lock<std::mutex> lock(markerstmpMutex);
    std::unique_lock<std::mutex>vmarkerslock(markersMutex);
    int sum=    (vmarkerstmp.size()+vmarkers.size());
    return sum;
}
void Calibrator::calibration_function(){

    while(keepCalibrating){
        calibrationMutex.lock();

        int nnewMarkers=0,nTotalViews=0;
        markerstmpMutex.lock();
        nnewMarkers=vmarkerstmp.size();
        if (vmarkerstmp.size()!=0){
            unique_lock<mutex> vmarkerslock(markersMutex);
            vmarkers.insert(vmarkers.end(),vmarkerstmp.begin(),vmarkerstmp.end());
            vmarkerstmp.clear();
            nTotalViews=vmarkers.size();
        }
        markerstmpMutex.unlock();

        if ( nnewMarkers!=0 && nTotalViews>=4){
            setInfoString("calibrating...");
             calibError=cameraCalibrate(vmarkers,_imageSize.width,_imageSize.height,_msize,_mmap,cameraParams);
            setInfoString("calibration error:"+std::to_string(calibError)+" using "+std::to_string(vmarkers.size())+" images");
        }
        calibrationMutex.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Calibrator::stopThread(){

    if (_calibThread.joinable()){
        keepCalibrating=false;
        _calibThread.join();
    }
}

void Calibrator::startThread(){
 if (_calibThread.joinable()) stopThread();
 keepCalibrating=true;
 _calibThread=std::thread([this]{this->calibration_function();});

}


inline cv::Point2f project_cv_(const cv::Point3f &p3d,const cv::Mat &cameraMatrix,const cv::Mat &RT,const cv::Mat & Distortion=cv::Mat()){
    assert(cameraMatrix.type()==CV_32F);
    assert(RT.type()==CV_32F);
    assert( (Distortion.type()==CV_32F &&  Distortion.total()>=5 )|| Distortion.total()==0);

    const float *cm=cameraMatrix.ptr<float>(0);
    const float *rt=RT.ptr<float>(0);
    const float *k=0;
    if ( Distortion.total()!=0) k=Distortion.ptr<float>(0);

    //project point first
    float x= p3d.x* rt[0]+p3d.y* rt[1]+p3d.z* rt[2]+rt[3];
    float y= p3d.x* rt[4]+p3d.y* rt[5]+p3d.z* rt[6]+rt[7];
    float z= p3d.x* rt[8]+p3d.y* rt[9]+p3d.z* rt[10]+rt[11];

    float xx=x/z;
    float yy=y/z;

    if (k!=0){//apply distortion //        k1,k2,p1,p2[,k3
        float r2=xx*xx+yy*yy;
        float r4=r2*r2;
        float comm=1+k[0]*r2+k[1]*r4+k[4]*(r4*r2);
        float xxx = xx * comm + 2*k[2] *xx*yy+ k[3]*(r2+2*xx*xx);
        float yyy= yy*comm+ k[2]*(r2+2*yy*yy)+2*k[3]*xx*yy;
        xx=xxx;
        yy=yyy;
    }
    return cv::Point2f((xx*cm[0])+cm[2],(yy*cm[4])+cm[5] );
}



float Calibrator::cameraCalibrate(std::vector<std::vector<aruco::Marker> >  &allMarkers, int imageWidth,int imageHeight,float markerSize,aruco::MarkerMap  &mmap, aruco::CameraParameters &iocam){

    // given the set of markers detected, the function determines the get the 2d-3d correspondes
    auto getMarker2d_3d_=[](std::vector<cv::Point2f>& p2d, vector<cv::Point3f>& p3d, const vector<Marker>& markers_detected,
            const MarkerMap& bc)
    {
        p2d.clear();
        p3d.clear();
        // for each detected marker
        for (size_t i = 0; i < markers_detected.size(); i++)
        {
            // find it in the bc
            auto fidx = std::string::npos;
            for (size_t j = 0; j < bc.size() && fidx == std::string::npos; j++)
                if (bc[j].id == markers_detected[i].id)
                    fidx = j;
            if (fidx != std::string::npos)
            {
                for (int j = 0; j < 4; j++)
                {
                    p2d.push_back(markers_detected[i][j]);
                    p3d.push_back(bc[fidx][j]);
                }
            }
        }
    };

    int calibflags=0;

    if (!iocam.isValid()){
        //first time
         iocam.CamSize = cv::Size(imageWidth,imageHeight);
    }
    else calibflags=    CV_CALIB_USE_INTRINSIC_GUESS;



    if (!mmap.isExpressedInMeters())
        mmap = mmap.convertToMeters(static_cast<float>( markerSize));



    vector<vector<cv::Point2f> >calib_p2d;
    vector<vector<cv::Point3f> > calib_p3d;

    for(auto &detected_markers:allMarkers){
        vector<cv::Point2f> p2d;
        vector<cv::Point3f> p3d;

        getMarker2d_3d_(p2d, p3d, detected_markers, mmap);
        if (p3d.size() > 0)
        {
            calib_p2d.push_back(p2d);
            calib_p3d.push_back(p3d);
        }
    }

    vector<cv::Mat> vr, vt;
    float err=cv::calibrateCamera(calib_p3d, calib_p2d, iocam.CamSize, iocam.CameraMatrix, iocam.Distorsion, vr, vt,calibflags);

    iocam.CameraMatrix.convertTo(iocam.CameraMatrix,CV_32F);
    iocam.Distorsion.convertTo(iocam.Distorsion,CV_32F);

return err;

     // cerr << "repj error=" << curRepjerr << endl;
 }

aruco::MarkerMap Calibrator::getDefaultCalibrationBoard(){



    unsigned char default_a4_board[] = {
      0x30, 0x20, 0x32, 0x34, 0x20, 0x31, 0x36, 0x31, 0x20, 0x34, 0x20, 0x2d,
      0x31, 0x30, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x2d, 0x35, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x2d, 0x35, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35,
      0x30, 0x30, 0x20, 0x30, 0x20, 0x32, 0x32, 0x37, 0x20, 0x34, 0x20, 0x2d,
      0x34, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x31, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x31, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x2d, 0x34, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x38, 0x35, 0x20, 0x34, 0x20, 0x32, 0x30, 0x30, 0x20, 0x2d, 0x31,
      0x30, 0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x2d, 0x31,
      0x30, 0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x2d, 0x31,
      0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x2d, 0x31,
      0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x36, 0x36, 0x20, 0x34, 0x20,
      0x38, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x31, 0x33, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x2d, 0x31, 0x35, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x32, 0x34, 0x34, 0x20, 0x34, 0x20, 0x2d, 0x31, 0x30, 0x30,
      0x30, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30,
      0x30, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30,
      0x30, 0x20, 0x2d, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x31, 0x30,
      0x30, 0x30, 0x20, 0x2d, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x34,
      0x34, 0x20, 0x34, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x2d, 0x34, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x2d, 0x34, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x2d, 0x39, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x2d, 0x39, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x39, 0x30, 0x20, 0x34, 0x20, 0x32, 0x30, 0x30, 0x20, 0x2d,
      0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x2d, 0x34,
      0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x2d, 0x39, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x2d, 0x39, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x32, 0x31, 0x34, 0x20, 0x34, 0x20, 0x38, 0x30, 0x30,
      0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30,
      0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30,
      0x20, 0x2d, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20,
      0x2d, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x35, 0x33, 0x20, 0x34,
      0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x2d, 0x35, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x2d, 0x35, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x37, 0x20, 0x34, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x32, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x31, 0x34, 0x33, 0x20, 0x34, 0x20, 0x32, 0x30, 0x30, 0x20, 0x32,
      0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x32, 0x31, 0x39, 0x20, 0x34, 0x20, 0x38, 0x30, 0x30, 0x20, 0x32,
      0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x32, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x2d, 0x33, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x37, 0x38, 0x20, 0x34, 0x20, 0x2d, 0x31, 0x30, 0x30,
      0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30, 0x30,
      0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30, 0x30, 0x20,
      0x33, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20,
      0x33, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x35, 0x39, 0x20, 0x34, 0x20,
      0x2d, 0x34, 0x30, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31,
      0x30, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x30, 0x30,
      0x20, 0x33, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20,
      0x33, 0x30, 0x30, 0x20, 0x30, 0x20, 0x32, 0x30, 0x39, 0x20, 0x34, 0x20,
      0x32, 0x30, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30,
      0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20,
      0x33, 0x30, 0x30, 0x20, 0x30, 0x20, 0x32, 0x30, 0x30, 0x20, 0x33, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x20, 0x34, 0x20, 0x38, 0x30, 0x30,
      0x20, 0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20,
      0x38, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x33,
      0x30, 0x30, 0x20, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x33, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x32, 0x34, 0x37, 0x20, 0x34, 0x20, 0x2d, 0x31, 0x30,
      0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35,
      0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35,
      0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x31, 0x30,
      0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x32, 0x33, 0x37,
      0x20, 0x34, 0x20, 0x2d, 0x34, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x31, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x2d, 0x34, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31,
      0x30, 0x30, 0x20, 0x34, 0x20, 0x32, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x32, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x36,
      0x20, 0x34, 0x20, 0x38, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x31, 0x34, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x38, 0x30, 0x30, 0x20, 0x39, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31,
      0x37, 0x37, 0x20, 0x34, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20, 0x32,
      0x30, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30, 0x30, 0x20, 0x32,
      0x30, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x35, 0x30, 0x30, 0x20, 0x31,
      0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x31, 0x30, 0x30, 0x30, 0x20,
      0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x39, 0x33, 0x20, 0x34, 0x20,
      0x2d, 0x34, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x31, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x30, 0x20, 0x30, 0x20, 0x31,
      0x30, 0x30, 0x20, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x2d, 0x34,
      0x30, 0x30, 0x20, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x38, 0x36,
      0x20, 0x34, 0x20, 0x32, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x30, 0x20,
      0x30, 0x20, 0x37, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30, 0x30, 0x20, 0x30,
      0x20, 0x37, 0x30, 0x30, 0x20, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20,
      0x32, 0x30, 0x30, 0x20, 0x31, 0x35, 0x30, 0x30, 0x20, 0x30, 0x20, 0x32,
      0x32, 0x39, 0x20, 0x34, 0x20, 0x38, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x32, 0x30, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x31, 0x33, 0x30, 0x30, 0x20, 0x31, 0x35, 0x30,
      0x30, 0x20, 0x30, 0x20, 0x38, 0x30, 0x30, 0x20, 0x31, 0x35, 0x30, 0x30,
      0x20, 0x30, 0x20, 0x41, 0x52, 0x55, 0x43, 0x4f, 0x5f, 0x4d, 0x49, 0x50,
      0x5f, 0x33, 0x36, 0x68, 0x31, 0x32
    };
    unsigned int default_a4_board_size = 1254;


        aruco::MarkerMap mmap;
       std::stringstream sstr;
       sstr.write((char*)default_a4_board, default_a4_board_size);
       mmap.fromStream(sstr);
       return mmap;
   }
}
