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
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/widget"
)

var filesData []FileData // Global slice to hold file data

type FileData struct {
	Path    string
	Hash    string
	Backups int
}

func main() {
	myApp := app.New()
	myWindow := myApp.NewWindow("BRODALF - Backup Explorer")
	myWindow.Resize(fyne.NewSize(800, 600))

	progressBar := widget.NewProgressBar()
	progressBar.Hide()

	hashingCheckbox := widget.NewCheck("Enable MD5 Hashing", nil)

	list := widget.NewList(
		func() int { return len(filesData) },
		func() fyne.CanvasObject {
			return container.NewHBox(widget.NewLabel("File Path"), widget.NewLabel("Hash"), widget.NewLabel("Backups"))
		},
		func(id widget.ListItemID, obj fyne.CanvasObject) {
			container := obj.(*fyne.Container)
			container.Objects[0].(*widget.Label).SetText(filepath.Base(filesData[id].Path))
			container.Objects[1].(*widget.Label).SetText(filesData[id].Hash)
			backupsText := "No"
			if filesData[id].Backups > 0 {
				backupsText = "Yes"
			}
			container.Objects[2].(*widget.Label).SetText(backupsText)
		},
	)

	listContainer := container.New(layout.NewMaxLayout(), list)

	buildIndexBtn := widget.NewButton("Build/Index Folder/File Structure", func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err != nil || uri == nil {
				return
			}
			indexedData, err := indexDirectory(uri.Path(), progressBar, hashingCheckbox)
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
			filesData = indexedData
			list.Refresh()
			if err := saveIndexToFile(filesData, "index.gz"); err != nil {
				dialog.ShowError(err, myWindow)
			}
		}, myWindow)
	})

	loadIndexBtn := widget.NewButton("Load the Index File", func() {
		dialog.ShowFileOpen(func(reader fyne.URIReadCloser, err error) {
			if err != nil || reader == nil {
				return
			}
			defer reader.Close()
			indexPath := reader.URI().Path()
			loadedData, err := readIndexFile(indexPath)
			if err != nil {
				dialog.ShowError(err, myWindow)
				return
			}
			filesData = loadedData
			list.Refresh()
		}, myWindow)
	})
	buttons := container.NewVBox(buildIndexBtn, loadIndexBtn)
	content := container.NewBorder(nil, nil, nil, buttons, listContainer)
	myWindow.SetContent(content)
	myWindow.ShowAndRun()
}

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

func indexDirectory(dirPath string, progressBar *widget.ProgressBar, hashingCheckbox *widget.Check) ([]FileData, error) {
	var filesData []FileData

	filepath.Walk(dirPath, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			var hash string
			if hashingCheckbox.Checked {
				var err error
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
		}
		return nil
	})

	return filesData, nil
}

func readIndexFile(indexPath string) ([]FileData, error) {
	file, err := os.Open(indexPath)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	gzReader, err := gzip.NewReader(file)
	if err != nil {
		return nil, err
	}
	defer gzReader.Close()

	var filesData []FileData
	decoder := json.NewDecoder(gzReader)
	err = decoder.Decode(&filesData)
	if err != nil {
		return nil, err
	}

	return filesData, nil
}
