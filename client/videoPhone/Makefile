
PROJECT_NAME =VideoPhone

TARGET_DIR  =../../bin
SOURCE_PATH =src
MAIN_CLASS  =$(SOURCE_PATH)/Phone/Main.as

TARGET_FILE =$(TARGET_DIR)/$(PROJECT_NAME).swf

build:
	mxmlc \
		-strict=false -debug=true \
		-output $(TARGET_FILE) \
		-source-path $(SOURCE_PATH) -- \
		$(MAIN_CLASS)

clean:
	rm -f $(TARGET_FILE)

