package main

import (
	"os"
	"path/filepath"

	"crypto/md5"
	"encoding/hex"

	"compress/gzip"
	"encoding/json"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

func saveIndexToFile(data []FileData, filename string) error {
	file, err := os.Create(filename)
	if err != nil {
		return err
	}
	defer file.Close()

	gzWriter := gzip.NewWriter(file)
	defer gzWriter.Close()

	encoder := json.NewEncoder(gzWriter)
	return encoder.Encode(data)
}

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

func indexDirectory(dirPath string, progressBar *widget.ProgressBar) ([]FileData, error) {
	var filesData []FileData

	totalFiles, _ := filepath.Glob(dirPath + "/*") // Just an approximation
	progressStep := 1.0 / float64(len(totalFiles))

	// Show and reset the progress bar
	progressBar.Show()
	progressBar.SetValue(0)

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
				Backups: 0,
			})
			progressBar.SetValue(progressBar.Value + progressStep)
		}
		return nil
	})

	progressBar.Hide() // Hide the progress bar when done
	return filesData, err
}
func main() {
	myApp := app.New()
	myWindow := myApp.NewWindow("BRODALF - Backup Explorer")
	myWindow.Resize(fyne.NewSize(800, 600))

	progressBar := widget.NewProgressBar()
	progressBar.Hide() // Initially hidden, will be shown during indexing

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

			// Index the selected directory
			filesData, err := indexDirectory(uri.Path(), progressBar)
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}

			// For now, save the index data to a file named "index.gz" in the current directory
			// This can be changed later based on user input or configuration
			err = saveIndexToFile(filesData, "index.gz")
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
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
		progressBar,
	)

	myWindow.SetContent(content)
	myWindow.ShowAndRun()
}
