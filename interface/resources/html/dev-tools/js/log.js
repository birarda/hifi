$(function(){

    function sanitizedMessageArray(index, message) {
        // pull out the time from the log entry
        var timeStart = message.indexOf('[') + 1;
        var timeEnd = message.indexOf(']');
        var time = message.substring(timeStart, timeEnd);

        // pull out the type from the log entry
        var typeStart = message.indexOf('[', timeStart + 1) + 1;
        var typeEnd = message.indexOf(']', timeEnd + 1);
        var type = message.substring(typeStart, typeEnd);

        return [time, type, message.substring(typeEnd + 1)];
    }

    // create a DataTable
    var table = $("#log").DataTable({
        data: [],
        columns: [
            { title: "Time" },
            { title: "Type" },
            { title: "Message" }
        ],
        bPaginate: false,
        ordering: false
    });

    // when we get a new log entry, sanitize it and add it to the table
    Developer.newLogLine.connect(function(index, message){
        if (index >= table.data.length) {
            table.row.add(sanitizedMessageArray(index, message)).draw(false);

            // window.scrollTo(0, document.body.scrollHeight);
        }
    });

    // enumerate the current log entries and set them up for DataTables
    $.each(Developer.log, function(index, message) {
        table.row.add(sanitizedMessageArray(index, message));
    });

    // change the column filter if the user asks for verbose debug
    $('#verbose-debug-checkbox').change(function(){
        if (this.checked) {
            // show the debug output in the table
            table.columns(2).search('').draw();
        } else {
            // hide the debug output in the table
            table.columns(2).search('^(?:(?!DEBUG).)*$', true).draw();
        }
    });

    // fire the change event on the verbose debug checkbox to start with the right value
    $('#verbose-debug-checkbox').change();
})
