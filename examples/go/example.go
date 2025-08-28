// Example usage of the InnoDB Reader Library in Go
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	
	"ibd-reader-example/ibd"
)

func printSeparator() {
	fmt.Println("========================================")
}

func exampleDecompressFile(inputFile, outputFile string) {
	fmt.Println()
	printSeparator()
	fmt.Println("Example: Decompress IBD File")
	printSeparator()
	
	reader, err := ibd.NewReader()
	if err != nil {
		log.Fatalf("Failed to create reader: %v", err)
	}
	defer reader.Close()
	
	// Enable debug mode for verbose output
	reader.SetDebug(true)
	
	fmt.Printf("Input file: %s\n", inputFile)
	fmt.Printf("Output file: %s\n", outputFile)
	fmt.Println("\nDecompressing...")
	
	err = reader.DecompressFile(inputFile, outputFile)
	if err != nil {
		log.Printf("Decompression failed: %v", err)
	} else {
		fmt.Println("\nDecompression successful!")
	}
}

func exampleDecompressPage() {
	fmt.Println()
	printSeparator()
	fmt.Println("Example: Decompress Single Page")
	printSeparator()
	
	reader, err := ibd.NewReader()
	if err != nil {
		log.Fatalf("Failed to create reader: %v", err)
	}
	defer reader.Close()
	
	// Simulate a compressed page (in reality, this would be read from file)
	compressedPage := make([]byte, 8192)
	
	decompressed, pageInfo, err := reader.DecompressPage(compressedPage)
	if err != nil {
		log.Printf("Page decompression failed: %v", err)
		return
	}
	
	fmt.Println("Page decompression successful!")
	fmt.Printf("  Decompressed size: %d bytes\n", len(decompressed))
	fmt.Printf("  Page number: %d\n", pageInfo.PageNumber)
	fmt.Printf("  Page type: %d (%s)\n", pageInfo.PageType, 
		ibd.GetPageTypeName(pageInfo.PageType))
	fmt.Printf("  Physical size: %d\n", pageInfo.PhysicalSize)
	fmt.Printf("  Logical size: %d\n", pageInfo.LogicalSize)
	fmt.Printf("  Is compressed: %v\n", pageInfo.IsCompressed)
	fmt.Printf("  Is encrypted: %v\n", pageInfo.IsEncrypted)
}

func exampleGetPageInfo() {
	fmt.Println()
	printSeparator()
	fmt.Println("Example: Get Page Information")
	printSeparator()
	
	// Simulate a page header
	pageHeader := make([]byte, 38)
	
	// Set page type at offset 24 (FIL_PAGE_TYPE)
	pageHeader[24] = 0x45  // 17855 >> 8
	pageHeader[25] = 0xBF  // 17855 & 0xFF (FIL_PAGE_INDEX)
	
	// Set page number at offset 4 (FIL_PAGE_OFFSET)
	pageHeader[7] = 42  // Page number 42
	
	pageInfo, err := ibd.GetPageInfo(pageHeader)
	if err != nil {
		log.Printf("Failed to get page info: %v", err)
		return
	}
	
	fmt.Println("Page information:")
	fmt.Printf("  Page number: %d\n", pageInfo.PageNumber)
	fmt.Printf("  Page type: %d (%s)\n", pageInfo.PageType,
		ibd.GetPageTypeName(pageInfo.PageType))
	fmt.Printf("  Is compressed: %v\n", pageInfo.IsCompressed)
	fmt.Printf("  Is encrypted: %v\n", pageInfo.IsEncrypted)
}

func exampleLibraryInfo() {
	fmt.Println()
	printSeparator()
	fmt.Println("InnoDB Reader Library Information")
	printSeparator()
	
	fmt.Printf("Library version: %s\n", ibd.GetVersion())
	fmt.Println("\nSupported page types:")
	
	pageTypes := []struct {
		value uint16
		name  string
	}{
		{ibd.PageTypeAllocated, "ALLOCATED"},
		{ibd.PageTypeUndoLog, "UNDO_LOG"},
		{ibd.PageTypeInode, "INODE"},
		{ibd.PageTypeFspHdr, "FSP_HDR"},
		{ibd.PageTypeIndex, "INDEX"},
		{ibd.PageTypeCompressed, "COMPRESSED"},
		{ibd.PageTypeEncrypted, "ENCRYPTED"},
	}
	
	for _, pt := range pageTypes {
		fmt.Printf("  %5d: %s\n", pt.value, ibd.GetPageTypeName(pt.value))
	}
}

func main() {
	// Parse command line flags
	var (
		inputFile  = flag.String("i", "", "Input IBD file")
		outputFile = flag.String("o", "", "Output file")
		decrypt    = flag.Bool("decrypt", false, "Decrypt the file")
		keyring    = flag.String("keyring", "", "Keyring file path")
		masterKey  = flag.Uint("master-key", 0, "Master key ID")
		serverUUID = flag.String("uuid", "", "Server UUID")
	)
	flag.Parse()
	
	fmt.Println("InnoDB Reader Library - Go Example")
	fmt.Println("===================================")
	
	// Initialize the library
	err := ibd.Init()
	if err != nil {
		log.Fatalf("Failed to initialize library: %v", err)
	}
	defer ibd.Cleanup()
	
	// Show library information
	exampleLibraryInfo()
	
	// Example: Get page information
	exampleGetPageInfo()
	
	// Example: Decompress a page
	exampleDecompressPage()
	
	// Handle file operations if arguments provided
	if *inputFile != "" && *outputFile != "" {
		if *decrypt {
			// Decrypt (and optionally decompress) file
			if *keyring == "" || *masterKey == 0 || *serverUUID == "" {
				fmt.Println("\nFor decryption, you need to provide:")
				fmt.Println("  -keyring <path>   Keyring file path")
				fmt.Println("  -master-key <id>  Master key ID")
				fmt.Println("  -uuid <uuid>      Server UUID")
				os.Exit(1)
			}
			
			reader, err := ibd.NewReader()
			if err != nil {
				log.Fatalf("Failed to create reader: %v", err)
			}
			defer reader.Close()
			
			reader.SetDebug(true)
			
			fmt.Printf("\nDecrypting and decompressing...\n")
			err = reader.DecryptAndDecompressFile(*inputFile, *outputFile, *keyring, 
				uint32(*masterKey), *serverUUID)
			if err != nil {
				log.Fatalf("Operation failed: %v", err)
			}
			fmt.Println("Operation successful!")
			
		} else {
			// Just decompress
			exampleDecompressFile(*inputFile, *outputFile)
		}
	} else {
		fmt.Println("\n")
		printSeparator()
		fmt.Println("File Operations")
		printSeparator()
		fmt.Println("To decompress a file:")
		fmt.Printf("  %s -i <input.ibd> -o <output.ibd>\n", os.Args[0])
		fmt.Println("\nTo decrypt and decompress a file:")
		fmt.Printf("  %s -decrypt -i <input.ibd> -o <output.ibd> \\\n", os.Args[0])
		fmt.Println("    -keyring <keyring_file> -master-key <id> -uuid <server_uuid>")
	}
}