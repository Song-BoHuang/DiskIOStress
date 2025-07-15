#!/bin/bash

# SCSI/UASP USB Device Detection and Testing Script

echo "==================================="
echo "SCSI/UASP USB Device Detection"
echo "==================================="

# Check for USB storage devices
echo "1. USB Storage Devices:"
lsusb | grep -i "storage\|mass"
echo ""

# Check for SCSI generic devices
echo "2. SCSI Generic Devices:"
if [ -e /dev/sg0 ]; then
    ls -la /dev/sg*
    echo ""

    # Check device information if sg_scan is available
    if command -v sg_scan >/dev/null 2>&1; then
        echo "3. SCSI Device Information:"
        sg_scan -i 2>/dev/null || echo "sg_scan failed or requires root permissions"
    else
        echo "3. sg_scan not available (install sg3-utils package)"
    fi
else
    echo "No SCSI generic devices found (/dev/sg*)"
fi
echo ""

# Check kernel messages for UAS/USB-Storage
echo "4. Kernel Driver Information:"
dmesg | tail -20 | grep -i "uas\|usb-storage\|scsi" | tail -5
echo ""

# Check if current engine is USB
echo "5. Current DiskIOStress Configuration:"
if [ -f "DiskIOStress.c" ]; then
    grep "#define DISK_IO_ENGINE" DiskIOStress.c
    echo ""

    # Check if compiled with SCSI support
    if nm DiskIOStress 2>/dev/null | grep -q "scsi_"; then
        echo "✅ DiskIOStress compiled with SCSI functions"
    else
        echo "❌ DiskIOStress not compiled or missing SCSI functions"
    fi
else
    echo "DiskIOStress.c not found in current directory"
fi
echo ""

echo "==================================="
echo "SCSI/UASP Compatibility Check"
echo "==================================="

# Check for required tools
echo "Required Tools Check:"
for tool in sg_scan sg_inq sg_readcap; do
    if command -v $tool >/dev/null 2>&1; then
        echo "✅ $tool - Available"
    else
        echo "❌ $tool - Missing (install sg3-utils)"
    fi
done
echo ""

# Usage instructions
echo "Usage Instructions:"
echo "1. Connect USB storage device with UASP support"
echo "2. Verify device appears as /dev/sg* (may require root)"
echo "3. Use './switch_io_engine.sh usb' to enable SCSI engine"
echo "4. Run DiskIOStress with appropriate device path"
echo ""
echo "Example:"
echo "  sudo ./DiskIOStress /dev/sg1 [options]"
echo ""
echo "Note: SCSI Generic access typically requires root permissions"
