package main

import (
	"fmt"
	"os"
	"path/filepath"

	"crypto/md5"
	"encoding/hex"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

func calculateMD5(filePath string) (string, error) {
	fileData, err := os.ReadFile(filePath)
	if err != nil {
		return "", err
	}
	hasher := md5.New()
	hasher.Write(fileData)
	return hex.EncodeToString(hasher.Sum(nil)), nil
}

// ListFiles returns a list of directories and files for a given path.
func ListFiles(dir string) ([]string, error) {
	var files []string
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		// To only add items from the root directory (and not subdirectories)
		if filepath.Dir(path) == dir {
			files = append(files, info.Name())
		}
		return nil
	})
	return files, err
}

type FileData struct {
	Path    string
	Hash    string
	Backups int // To track the number of backups for this file
}

func indexDirectory(dirPath string) ([]FileData, error) {
	var filesData []FileData

	err := filepath.Walk(dirPath, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			hash, err := calculateMD5(path)
			if err != nil {
				return err
			}
			filesData = append(filesData, FileData{
				Path:    path,
				Hash:    hash,
				Backups: 0, // We'll update this later when implementing backup functionality
			})
		}
		return nil
	})

	if err != nil {
		return nil, err
	}
	return filesData, nil
}
func main() {
	myApp := app.New()
	myWindow := myApp.NewWindow("BRODALF - Backup Explorer")
	myWindow.Resize(fyne.NewSize(800, 600))

	// Button to build/index folder/file structure
	buildIndexBtn := widget.NewButton("Build/Index Folder/File Structure", func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
			if uri == nil {
				// User canceled or closed the dialog without selecting a directory
				return
			}

			// TODO: Use the selected directory (uri) to build and index the folder/file structure
			fmt.Println("Selected directory:", uri.String())
		}, myWindow)

	})

	// Button to load the index file
	loadIndexBtn := widget.NewButton("Load the Index File", func() {
		// TODO: Implement the functionality to load the index file
	})

	// Layout the buttons vertically
	content := container.NewVBox(
		buildIndexBtn,
		loadIndexBtn,
	)

	myWindow.SetContent(content)
	myWindow.ShowAndRun()
}
