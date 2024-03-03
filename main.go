package main

import (
	"compress/gzip"
	"crypto/md5"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"

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

type FileData struct {
	Path    string
	Hash    string
	Backups int
}

func indexDirectory(dirPath string, progressBar *widget.ProgressBar, hashingCheckbox *widget.Check) ([]FileData, error) {
	var filesData []FileData

	totalFiles, _ := filepath.Glob(dirPath + "/*")
	progressStep := 1.0 / float64(len(totalFiles))

	progressBar.Show()
	progressBar.SetValue(0)

	err := filepath.Walk(dirPath, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			var hash string
			if hashingCheckbox.Checked {
				hash, err = calculateMD5(path)
				if err != nil {
					return err
				}
			} else {
				hash = "Not Calculated"
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

	progressBar.Hide()
	return filesData, err
}

func main() {
	myApp := app.New()
	myWindow := myApp.NewWindow("BRODALF - Backup Explorer")
	myWindow.Resize(fyne.NewSize(800, 600))

	progressBar := widget.NewProgressBar()
	progressBar.Hide()

	hashingCheckbox := widget.NewCheck("Enable MD5 Hashing", nil)

	buildIndexBtn := widget.NewButton("Build/Index Folder/File Structure", func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
			if uri == nil {
				return
			}
			filesData, err := indexDirectory(uri.Path(), progressBar, hashingCheckbox)
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
			err = saveIndexToFile(filesData, "index.gz")
			if err != nil {
				dialog.ShowError(err, myWindow)
			}
		}, myWindow)
	})

	loadIndexBtn := widget.NewButton("Load the Index File", func() {
		// TODO: Implement loading functionality
	})

	content := container.NewVBox(buildIndexBtn, loadIndexBtn, hashingCheckbox, progressBar)
	myWindow.SetContent(content)
	myWindow.ShowAndRun()
}
