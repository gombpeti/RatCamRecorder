// camfeatures -- read-only interrogation of what a camera actually supports.
//
// Written because a .pfs only records what was set, not what is available, so
// the absence of Binning entries there does not prove the camera lacks it.
// This asks the camera itself.
//
//   camfeatures                 every camera
//   camfeatures --serial 41975154

#include <pylon/PylonIncludes.h>
#include <pylon/ParameterIncludes.h>

#include <cstdio>
#include <string>

using namespace Pylon;

namespace {

void ShowInteger(GenApi::INodeMap& nm, const char* name) {
    try {
        CIntegerParameter p(nm, name);
        if (!p.IsReadable()) { std::printf("  %-26s not available\n", name); return; }
        std::printf("  %-26s = %lld   [%lld..%lld step %lld]%s\n", name,
                    (long long)p.GetValue(), (long long)p.GetMin(),
                    (long long)p.GetMax(), (long long)p.GetInc(),
                    p.IsWritable() ? "" : "   (read-only)");
    } catch (const GenericException&) {
        std::printf("  %-26s not available\n", name);
    }
}

void ShowEnum(GenApi::INodeMap& nm, const char* name) {
    try {
        CEnumParameter p(nm, name);
        if (!p.IsReadable()) { std::printf("  %-26s not available\n", name); return; }
        std::printf("  %-26s = %-14s  allowed:", name, p.GetValue().c_str());
        GenApi::StringList_t entries;
        p.GetSettableValues(entries);
        for (size_t i = 0; i < entries.size(); ++i)
            std::printf(" %s", entries[i].c_str());
        std::printf("\n");
    } catch (const GenericException&) {
        std::printf("  %-26s not available\n", name);
    }
}

void ShowFloat(GenApi::INodeMap& nm, const char* name) {
    try {
        CFloatParameter p(nm, name);
        if (!p.IsReadable()) { std::printf("  %-26s not available\n", name); return; }
        std::printf("  %-26s = %.3f   [%.3f..%.3f]\n", name,
                    p.GetValue(), p.GetMin(), p.GetMax());
    } catch (const GenericException&) {
        std::printf("  %-26s not available\n", name);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string wanted, pfs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--serial" && i + 1 < argc) wanted = argv[++i];
        else if (a == "--pfs" && i + 1 < argc) pfs = argv[++i];
    }

    PylonAutoInitTerm autoInit;
    try {
        DeviceInfoList_t devices;
        CTlFactory::GetInstance().EnumerateDevices(devices);

        for (size_t i = 0; i < devices.size(); ++i) {
            const std::string serial = devices[i].GetSerialNumber().c_str();
            if (!wanted.empty() && serial != wanted) continue;

            CInstantCamera cam(CTlFactory::GetInstance().CreateDevice(devices[i]));
            cam.Open();
            GenApi::INodeMap& nm = cam.GetNodeMap();

            // Report what the camera says AFTER the settings are applied --
            // its own numbers, not arithmetic done from outside.
            if (!pfs.empty()) {
                try { CFeaturePersistence::Load(pfs.c_str(), &nm, false); }
                catch (const GenericException& e) {
                    std::printf("(.pfs load: %s)\n", e.GetDescription());
                }
            }

            std::printf("\n=== %s  serial %s  (%s) ===\n",
                        devices[i].GetModelName().c_str(), serial.c_str(),
                        devices[i].GetUserDefinedName().c_str());

            std::printf("\n-- current ROI --\n");
            for (const char* n : {"Width", "Height", "OffsetX", "OffsetY",
                                  "WidthMax", "HeightMax",
                                  "SensorWidth", "SensorHeight"})
                ShowInteger(nm, n);

            std::printf("\n-- binning (sums pixels: less data AND more light) --\n");
            for (const char* n : {"BinningHorizontal", "BinningVertical"})
                ShowInteger(nm, n);
            for (const char* n : {"BinningHorizontalMode", "BinningVerticalMode"})
                ShowEnum(nm, n);

            std::printf("\n-- decimation (skips pixels: less data, same light) --\n");
            for (const char* n : {"DecimationHorizontal", "DecimationVertical"})
                ShowInteger(nm, n);

            std::printf("\n-- pixel format and rate --\n");
            ShowEnum(nm, "PixelFormat");
            ShowEnum(nm, "BslSensorBitDepthMode");
            ShowEnum(nm, "BslSensorBitDepth");
            // These say where the time actually goes.
            for (const char* n : {"SensorReadoutTime", "ExposureOverlapTimeMax",
                                  "BslExposureStartDelay", "TriggerDelay"})
                ShowFloat(nm, n);
            for (const char* n : {"BslAcquisitionBurstMode", "ExposureMode",
                                  "TriggerMode", "SensorReadoutMode",
                                  "BslMultipleROIRowsEnable"})
                ShowEnum(nm, n);
            ShowFloat(nm, "ExposureTime");
            ShowFloat(nm, "AcquisitionFrameRate");
            // The camera's own view of what it can sustain, which beats any
            // arithmetic done from the outside.
            ShowFloat(nm, "ResultingFrameRate");
            ShowFloat(nm, "BslResultingTransferFrameRate");
            ShowInteger(nm, "DeviceLinkThroughputLimit");
            ShowInteger(nm, "DeviceLinkCurrentThroughput");
            ShowEnum(nm, "DeviceLinkThroughputLimitMode");

            cam.Close();
        }
    } catch (const GenericException& e) {
        std::printf("pylon exception: %s\n", e.GetDescription());
        return 1;
    }
    return 0;
}
