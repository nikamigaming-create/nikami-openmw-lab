---
-- Provides read-only access to data directories via VFS.
-- Interface is very similar to "io" library.
<<<<<<< HEAD
-- @context global|menu|local|player|load
=======
-- @context global|menu|local|player
>>>>>>> origin/main
-- @module vfs
-- @usage local vfs = require('openmw.vfs')



---
-- @type FileHandle
-- @field #string fileName VFS path to related file

---
-- Close a file handle
-- @function [parent=#FileHandle] close
-- @param self
-- @return #boolean true if a call succeeds without errors.
-- @return #nil, #string nil plus the error message in case of any error.

---
<<<<<<< HEAD
-- Get an iterator function to fetch the next line from a given file.
-- Throws an exception if the file is closed.
=======
-- Get an iterator function to fetch the next line from given file.
-- Throws an exception if file is closed.
>>>>>>> origin/main
--
-- Hint: since garbage collection works once per frame,
-- you will get the whole file in RAM if you read it in one frame.
-- So if you need to read a really large file, it is better to split reading
-- between different frames (e.g. by keeping a current position in file
-- and using a "seek" to read from saved position).
-- @function [parent=#FileHandle] lines
-- @param self
-- @return #function Iterator function to get next line
-- @usage f = vfs.open("Test\\test.txt");
-- for line in f:lines() do
--     print(line);
-- end

---
<<<<<<< HEAD
-- Set new position in a file.
-- Throws an exception if the file is closed or seek base is incorrect.
=======
-- Set new position in file.
-- Throws an exception if file is closed or seek base is incorrect.
>>>>>>> origin/main
-- @function [parent=#FileHandle] seek
-- @param self
-- @param #string whence Seek base (optional, "cur" by default). Can be:
--
--   * "set" - seek from beginning of file;
--   * "cur" - seek from current position;
--   * "end" - seek from end of file (offset needs to be <= 0);
-- @param #number offset Offset from given base (optional, 0 by default)
-- @return #number new position in file if a call succeeds without errors.
-- @return #nil, #string nil plus the error message in case of any error.
-- @usage -- set pointer to beginning of file
-- f = vfs.open("Test\\test.txt");
-- f:seek("set");
-- @usage -- print current position in file
-- f = vfs.open("Test\\test.txt");
-- print(f:seek());
-- @usage -- print file size
-- f = vfs.open("Test\\test.txt");
-- print(f:seek("end"));

---
<<<<<<< HEAD
-- Read data from a file to strings.
-- Throws an exception if the file is closed, if there are too many arguments or if an invalid format is encountered.
=======
-- Read data from file to strings.
-- Throws an exception if file is closed, if there is too many arguments or if an invalid format encountered.
>>>>>>> origin/main
--
-- Hint: since garbage collection works once per frame,
-- you will get the whole file in RAM if you read it in one frame.
-- So if you need to read a really large file, it is better to split reading
-- between different frames (e.g. by keeping a current position in file
-- and using a "seek" to read from saved position).
-- @function [parent=#FileHandle] read
-- @param self
-- @param ... Read formats (up to 20 arguments, default value is one "*l"). Can be:
--
--   * "\*a" (or "*all") - reads the whole file, starting at the current position as #string. On end of file, it returns the empty string.
--   * "\*l" (or "*line") - reads the next line (skipping the end of line), returning nil on end of file (nil and error message if error occured);
--   * "\*n" (or "*number") - read a floating point value as #number (nil and error message if error occured);
--   * number - reads a #string with up to this number of characters, returning nil on end of file (nil and error message if error occured). If number is 0 and end of file is not reached, it reads nothing and returns an empty string;
-- @return #string One #string for every format if a call succeeds without errors. One #string for every successfully handled format, nil for first failed format.
-- @usage -- read three numbers from file
-- f = vfs.open("Test\\test.txt");
-- local n1, n2, n3 = f:read("*number", "*number", "*number");
-- @usage -- read 10 bytes from file
-- f = vfs.open("Test\\test.txt");
-- local n4 = f:read(10);
-- @usage -- read until end of file
-- f = vfs.open("Test\\test.txt");
-- local n5 = f:read("*all");
-- @usage -- read a line from file
-- f = vfs.open("Test\\test.txt");
-- local n6 = f:read();
-- @usage -- try to read three numbers from file with "1" content
-- f = vfs.open("one.txt");
-- print(f:read("*number", "*number", "*number"));
-- -- prints(1, nil)

---
<<<<<<< HEAD
-- Check if a file exists in VFS
=======
-- Check if file exists in VFS
>>>>>>> origin/main
-- @function [parent=#vfs] fileExists
-- @param #string fileName Path to file in VFS
-- @return #boolean (true - exists, false - does not exist)
-- @usage local exists = vfs.fileExists("Test\\test.txt");

---
-- Open a file
-- @function [parent=#vfs] open
-- @param #string fileName Path to file in VFS
-- @return #FileHandle Opened file handle if a call succeeds without errors.
-- @return #nil, #string nil plus the error message in case of any error.
-- @usage f, msg = vfs.open("Test\\test.txt");
-- -- print file name or error message
-- if (f == nil)
--     print(msg);
-- else
--     print(f.fileName);
-- end

---
<<<<<<< HEAD
-- Get an iterator function to fetch the next line from a file with the given path.
-- Throws an exception if the file is closed or the file with the given path does not exist.
-- Closes the file automatically when it fails to read any more bytes.
=======
-- Get an iterator function to fetch the next line from file with given path.
-- Throws an exception if file is closed or file with given path does not exist.
-- Closes file automatically when it fails to read any more bytes.
>>>>>>> origin/main
--
-- Hint: since garbage collection works once per frame,
-- you will get the whole file in RAM if you read it in one frame.
-- So if you need to read a really large file, it is better to split reading
-- between different frames (e.g. by keeping a current position in file
-- and using a "seek" to read from saved position).
-- @function [parent=#vfs] lines
-- @param #string fileName Path to file in VFS
-- @return #function Iterator function to get next line
-- @usage for line in vfs.lines("Test\\test.txt") do
--     print(line);
-- end

---
<<<<<<< HEAD
-- Get an iterator function to fetch file names with given path prefix from the VFS
=======
-- Get iterator function to fetch file names with given path prefix from VFS
>>>>>>> origin/main
-- @function [parent=#vfs] pathsWithPrefix
-- @param #string path Path prefix
-- @return #function Function to get next file name
-- @usage -- get all files with given prefix from VFS index
-- for fileName in vfs.pathsWithPrefix("Music\\Explore") do
--     print(fileName);
-- end
-- @usage -- get some first files
-- local getNextFile = vfs.pathsWithPrefix("Music\\Explore");
-- local firstFile = getNextFile();
-- local secondFile = getNextFile();

---
-- Detect a file handle type
-- @function [parent=#vfs] type
-- @param #any handle Object to check
-- @return #string File handle type. Can be:
--
--   * "file" - an argument is a valid opened @{openmw.vfs#FileHandle};
--   * "closed file" - an argument is a valid closed @{openmw.vfs#FileHandle};
--   * nil - an argument is not a @{openmw.vfs#FileHandle};
-- @usage f = vfs.open("Test\\test.txt");
-- print(vfs.type(f));

return nil
