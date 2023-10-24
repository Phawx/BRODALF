package main

import (
	"fmt"
	"os"
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

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
