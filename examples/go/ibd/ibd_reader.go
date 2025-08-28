// Package ibd provides Go bindings for the InnoDB Reader Library
package ibd

/*
#cgo CFLAGS: -I../../../lib
#cgo LDFLAGS: -L../../../build -libd_reader -Wl,-rpath,../../../build
#include "ibd_reader_api.h"
#include <stdlib.h>
*/
import "C"
import (
	"errors"
	"fmt"
	"unsafe"
)

// Result codes
const (
	Success                     = C.IBD_SUCCESS
	ErrorInvalidParam          = C.IBD_ERROR_INVALID_PARAM
	ErrorFileNotFound          = C.IBD_ERROR_FILE_NOT_FOUND
	ErrorFileRead              = C.IBD_ERROR_FILE_READ
	ErrorFileWrite             = C.IBD_ERROR_FILE_WRITE
	ErrorInvalidFormat         = C.IBD_ERROR_INVALID_FORMAT
	ErrorCompression           = C.IBD_ERROR_COMPRESSION
	ErrorDecompression         = C.IBD_ERROR_DECOMPRESSION
	ErrorEncryption            = C.IBD_ERROR_ENCRYPTION
	ErrorDecryption            = C.IBD_ERROR_DECRYPTION
	ErrorMemory                = C.IBD_ERROR_MEMORY
	ErrorNotImplemented        = C.IBD_ERROR_NOT_IMPLEMENTED
	ErrorKeyring              = C.IBD_ERROR_KEYRING
	ErrorUnknown              = C.IBD_ERROR_UNKNOWN
)

// Page types
const (
	PageTypeAllocated              = C.IBD_PAGE_TYPE_ALLOCATED
	PageTypeUndoLog                = C.IBD_PAGE_UNDO_LOG
	PageTypeInode                  = C.IBD_PAGE_INODE
	PageTypeIbufFreeList           = C.IBD_PAGE_IBUF_FREE_LIST
	PageTypeIbufBitmap             = C.IBD_PAGE_IBUF_BITMAP
	PageTypeSys                    = C.IBD_PAGE_TYPE_SYS
	PageTypeTrxSys                 = C.IBD_PAGE_TYPE_TRX_SYS
	PageTypeFspHdr                 = C.IBD_PAGE_TYPE_FSP_HDR
	PageTypeXdes                   = C.IBD_PAGE_TYPE_XDES
	PageTypeBlob                   = C.IBD_PAGE_TYPE_BLOB
	PageTypeZblob                  = C.IBD_PAGE_TYPE_ZBLOB
	PageTypeZblob2                 = C.IBD_PAGE_TYPE_ZBLOB2
	PageTypeCompressed             = C.IBD_PAGE_COMPRESSED
	PageTypeEncrypted              = C.IBD_PAGE_ENCRYPTED
	PageTypeCompressedAndEncrypted = C.IBD_PAGE_COMPRESSED_AND_ENCRYPTED
	PageTypeEncryptedRtree         = C.IBD_PAGE_ENCRYPTED_RTREE
	PageTypeIndex                  = C.IBD_PAGE_INDEX
)

// PageInfo contains information about an InnoDB page
type PageInfo struct {
	PageNumber   uint32
	PageType     uint16
	PhysicalSize int
	LogicalSize  int
	IsCompressed bool
	IsEncrypted  bool
}

// Reader represents an InnoDB reader context
type Reader struct {
	handle C.ibd_reader_t
}

// Init initializes the InnoDB reader library
func Init() error {
	result := C.ibd_init()
	if result != Success {
		return fmt.Errorf("failed to initialize library: %d", result)
	}
	return nil
}

// Cleanup cleans up the InnoDB reader library
func Cleanup() {
	C.ibd_cleanup()
}

// GetVersion returns the library version string
func GetVersion() string {
	return C.GoString(C.ibd_get_version())
}

// NewReader creates a new InnoDB reader
func NewReader() (*Reader, error) {
	handle := C.ibd_reader_create()
	if handle == nil {
		return nil, errors.New("failed to create reader")
	}
	return &Reader{handle: handle}, nil
}

// Close destroys the reader and frees resources
func (r *Reader) Close() {
	if r.handle != nil {
		C.ibd_reader_destroy(r.handle)
		r.handle = nil
	}
}

// GetError returns the last error message from the reader
func (r *Reader) GetError() string {
	if r.handle == nil {
		return "reader is closed"
	}
	return C.GoString(C.ibd_reader_get_error(r.handle))
}

// SetDebug enables or disables debug mode
func (r *Reader) SetDebug(enable bool) {
	if r.handle != nil {
		if enable {
			C.ibd_reader_set_debug(r.handle, 1)
		} else {
			C.ibd_reader_set_debug(r.handle, 0)
		}
	}
}

// DecompressFile decompresses an entire IBD file
func (r *Reader) DecompressFile(inputPath, outputPath string) error {
	if r.handle == nil {
		return errors.New("reader is closed")
	}
	
	cInput := C.CString(inputPath)
	cOutput := C.CString(outputPath)
	defer C.free(unsafe.Pointer(cInput))
	defer C.free(unsafe.Pointer(cOutput))
	
	result := C.ibd_decompress_file(r.handle, cInput, cOutput)
	if result != Success {
		return fmt.Errorf("decompression failed: %s (code %d)", r.GetError(), result)
	}
	
	return nil
}

// DecompressPage decompresses a single page in memory
func (r *Reader) DecompressPage(compressed []byte) ([]byte, *PageInfo, error) {
	if len(compressed) == 0 {
		return nil, nil, errors.New("empty input")
	}
	
	// Allocate output buffer (typically 2x input size for compression)
	decompressed := make([]byte, len(compressed)*2)
	decompressedSize := C.size_t(len(decompressed))
	
	var cPageInfo C.ibd_page_info_t
	
	result := C.ibd_decompress_page(
		r.handle,
		(*C.uint8_t)(unsafe.Pointer(&compressed[0])),
		C.size_t(len(compressed)),
		(*C.uint8_t)(unsafe.Pointer(&decompressed[0])),
		&decompressedSize,
		&cPageInfo,
	)
	
	if result != Success {
		return nil, nil, fmt.Errorf("page decompression failed: code %d", result)
	}
	
	// Resize output to actual size
	decompressed = decompressed[:decompressedSize]
	
	pageInfo := &PageInfo{
		PageNumber:   uint32(cPageInfo.page_number),
		PageType:     uint16(cPageInfo.page_type),
		PhysicalSize: int(cPageInfo.physical_size),
		LogicalSize:  int(cPageInfo.logical_size),
		IsCompressed: cPageInfo.is_compressed != 0,
		IsEncrypted:  cPageInfo.is_encrypted != 0,
	}
	
	return decompressed, pageInfo, nil
}

// DecryptFile decrypts an entire IBD file
func (r *Reader) DecryptFile(inputPath, outputPath, keyringPath string, masterKeyID uint32, serverUUID string) error {
	if r.handle == nil {
		return errors.New("reader is closed")
	}
	
	cInput := C.CString(inputPath)
	cOutput := C.CString(outputPath)
	cKeyring := C.CString(keyringPath)
	cUUID := C.CString(serverUUID)
	defer C.free(unsafe.Pointer(cInput))
	defer C.free(unsafe.Pointer(cOutput))
	defer C.free(unsafe.Pointer(cKeyring))
	defer C.free(unsafe.Pointer(cUUID))
	
	result := C.ibd_decrypt_file(r.handle, cInput, cOutput, cKeyring, C.uint32_t(masterKeyID), cUUID)
	if result != Success {
		return fmt.Errorf("decryption failed: %s (code %d)", r.GetError(), result)
	}
	
	return nil
}

// DecryptAndDecompressFile decrypts and decompresses an IBD file in one operation
func (r *Reader) DecryptAndDecompressFile(inputPath, outputPath, keyringPath string, masterKeyID uint32, serverUUID string) error {
	if r.handle == nil {
		return errors.New("reader is closed")
	}
	
	cInput := C.CString(inputPath)
	cOutput := C.CString(outputPath)
	cKeyring := C.CString(keyringPath)
	cUUID := C.CString(serverUUID)
	defer C.free(unsafe.Pointer(cInput))
	defer C.free(unsafe.Pointer(cOutput))
	defer C.free(unsafe.Pointer(cKeyring))
	defer C.free(unsafe.Pointer(cUUID))
	
	result := C.ibd_decrypt_and_decompress_file(r.handle, cInput, cOutput, cKeyring, C.uint32_t(masterKeyID), cUUID)
	if result != Success {
		return fmt.Errorf("operation failed: %s (code %d)", r.GetError(), result)
	}
	
	return nil
}

// GetPageInfo gets information from a page buffer
func GetPageInfo(pageData []byte) (*PageInfo, error) {
	if len(pageData) < 38 { // Minimum page header size
		return nil, errors.New("page data too small")
	}
	
	var cPageInfo C.ibd_page_info_t
	
	result := C.ibd_get_page_info(
		(*C.uint8_t)(unsafe.Pointer(&pageData[0])),
		C.size_t(len(pageData)),
		&cPageInfo,
	)
	
	if result != Success {
		return nil, fmt.Errorf("failed to get page info: code %d", result)
	}
	
	return &PageInfo{
		PageNumber:   uint32(cPageInfo.page_number),
		PageType:     uint16(cPageInfo.page_type),
		PhysicalSize: int(cPageInfo.physical_size),
		LogicalSize:  int(cPageInfo.logical_size),
		IsCompressed: cPageInfo.is_compressed != 0,
		IsEncrypted:  cPageInfo.is_encrypted != 0,
	}, nil
}

// IsPageCompressed checks if a page is compressed based on its header
func IsPageCompressed(pageData []byte, physicalSize, logicalSize int) bool {
	if len(pageData) == 0 {
		return false
	}
	
	result := C.ibd_is_page_compressed(
		(*C.uint8_t)(unsafe.Pointer(&pageData[0])),
		C.size_t(physicalSize),
		C.size_t(logicalSize),
	)
	
	return result != 0
}

// GetPageTypeName returns the human-readable name for a page type
func GetPageTypeName(pageType uint16) string {
	return C.GoString(C.ibd_get_page_type_name(C.uint16_t(pageType)))
}