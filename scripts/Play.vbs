' Start the selector without a command window, with optional CLI arguments.
Option Explicit
Dim shell, files, folder, command, argument
Set shell = CreateObject("WScript.Shell")
Set files = CreateObject("Scripting.FileSystemObject")
folder = files.GetParentFolderName(WScript.ScriptFullName)
shell.CurrentDirectory = folder
command = QuoteArgument(files.BuildPath(folder, "q2psx.exe"))
For Each argument In WScript.Arguments
    command = command & " " & QuoteArgument(argument)
Next
shell.Run command, 1, False

Function QuoteArgument(value)
    Dim result, slashes, i, character
    result = Chr(34)
    slashes = 0
    For i = 1 To Len(value)
        character = Mid(value, i, 1)
        If character = "\" Then
            slashes = slashes + 1
        Else
            If character = Chr(34) Then
                result = result & String(slashes * 2 + 1, "\") & character
            Else
                result = result & String(slashes, "\") & character
            End If
            slashes = 0
        End If
    Next
    QuoteArgument = result & String(slashes * 2, "\") & Chr(34)
End Function
