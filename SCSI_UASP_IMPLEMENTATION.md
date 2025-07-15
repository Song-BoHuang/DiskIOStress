# SCSI/UASP USB Engine Implementation

## Overview
Enhanced the `IO_ENGINE_USB` to use proper SCSI/UASP (USB Attached SCSI Protocol) commands for USB storage devices. This provides direct SCSI command access through USB, offering better performance and compatibility with modern USB storage devices.

## SCSI/UASP Implementation Features

### ✅ Complete SCSI Command Set
- **READ(16)**: High performance block read operations
- **WRITE(16)**: High performance block write operations
- **WRITE UNCORRECTABLE(16)**: Uncorrectable error injection
- **SYNCHRONIZE CACHE(10)**: Force cache flush to storage
- **UNMAP**: SCSI TRIM/discard operation
- **INQUIRY**: Device identification and capabilities
- **READ CAPACITY(16)**: Get device size and block size
- **TEST UNIT READY**: Device readiness check

### ✅ UASP Optimizations
- **Direct SG_IO Interface**: Bypasses filesystem layer for direct device access
- **Large LBA Support**: 64-bit LBA addressing for >2TB devices
- **Timeout Management**: 20-second timeouts for reliable USB operations
- **Error Handling**: Proper SCSI sense data processing

### ✅ USB-Specific Features
- **SCSI Generic (sg) Interface**: Uses `/dev/sg*` devices for direct SCSI access
- **Big Endian Protocol**: Proper SCSI command byte ordering
- **Parameter Lists**: Structured SCSI command parameters
- **Sense Buffer Handling**: Complete error information retrieval

## Technical Implementation

### SCSI Command Structure
All SCSI commands follow proper SCSI-3 specifications:

```c
struct sg_io_hdr io_hdr;
unsigned char cdb[16];        // Command Descriptor Block
unsigned char sense_buffer[32]; // Error information
```

### LBA Addressing
Supports full 64-bit LBA range for modern large capacity USB drives:
```c
// LBA encoding (8 bytes, big endian)
cdb[2] = (lba >> 56) & 0xff;
cdb[3] = (lba >> 48) & 0xff;
// ... continuing for all 8 bytes
```

### Data Transfer
Direct memory mapping for optimal performance:
```c
io_hdr.dxfer_direction = SG_DXFER_FROM_DEV; // or SG_DXFER_TO_DEV
io_hdr.dxfer_len = len * block_size;
io_hdr.dxferp = data_buffer;
```

## SCSI Function Implementations

### `scsi_read()`
- Uses READ(16) command (0x88)
- Supports up to 4GB transfer per command
- Direct DMA transfer to user buffer

### `scsi_write()`
- Uses WRITE(16) command (0x8A)
- Supports up to 4GB transfer per command
- Direct DMA transfer from user buffer

### `scsi_writeuncor()`
- Uses WRITE UNCORRECTABLE(16) command (0x9E)
- Creates intentional uncorrectable errors
- Useful for error handling testing

### `scsi_flush()`
- Uses SYNCHRONIZE CACHE(10) command (0x35)
- Forces all cached data to storage media
- Essential for data integrity

### `scsi_trim()`
- Uses UNMAP command (0x42)
- Deallocates unused blocks
- Improves SSD performance and wear leveling

### `scsi_inquiry()`
- Uses INQUIRY command (0x12)
- Returns device identification information
- Vendor, product, and revision strings

### `scsi_read_capacity()`
- Uses READ CAPACITY(16) command (0x9E/0x10)
- Returns total device capacity and block size
- Essential for device size detection

### `scsi_test_unit_ready()`
- Uses TEST UNIT READY command (0x00)
- Checks if device is ready for operations
- No data transfer, status only

## Usage Requirements

### Device Access
USB storage devices using SCSI/UASP typically appear as:
- `/dev/sg*` devices for SCSI Generic interface
- Requires appropriate permissions (usually root or sg group)

### Kernel Support
Requires the following kernel modules:
- `sg` - SCSI Generic driver
- `usb-storage` or `uas` - USB storage drivers
- `scsi_mod` - Core SCSI subsystem

### Compilation
Automatically includes required headers:
```c
#include <scsi/sg.h>     // SCSI Generic interface
#include <scsi/scsi.h>   // SCSI command definitions
```

## Performance Benefits

### vs Standard File I/O
- **Direct Device Access**: Bypasses filesystem overhead
- **Large Transfer Support**: Multi-megabyte transfers in single commands
- **Lower CPU Usage**: DMA transfers reduce CPU involvement
- **Better Error Information**: Detailed SCSI sense data

### vs Block Device I/O
- **Command Queuing**: UASP supports native command queuing
- **Better Protocol**: More efficient than Bulk-Only Transport (BOT)
- **Advanced Features**: TRIM, NCQ, error injection support
- **Future Proof**: Standards-based approach

## Error Handling

### SCSI Status Codes
- **0x00**: Good - Command completed successfully
- **0x02**: Check Condition - Error occurred, check sense data
- **0x04**: Condition Met - Command completed with condition
- **0x08**: Busy - Device temporarily unavailable

### Sense Data Processing
Comprehensive error information including:
- Sense Key (error category)
- Additional Sense Code (specific error)
- Error location and recovery information

The SCSI/UASP USB engine provides enterprise-grade storage testing capabilities for USB devices while maintaining compatibility with the existing DiskIOStress framework!
