$(function(){

    $('.button-checkbox').each(function () {
        // Settings
        var $widget = $(this),
            $button = $widget.find('button'),
            $checkbox = $widget.find('input:checkbox'),
            color = $button.data('color'),
            settings = {
                on: {
                    icon: 'glyphicon glyphicon-check'
                },
                off: {
                    icon: 'glyphicon glyphicon-unchecked'
                }
            };

        // Event Handlers
        $button.on('click', function() {
            $checkbox.prop('checked', !$checkbox.is(':checked'));
            $checkbox.triggerHandler('change');
            updateDisplay();
        });
        $checkbox.on('change', function() {
            updateDisplay();
        });

        // Actions
        function updateDisplay() {
            var isChecked = $checkbox.is(':checked');

            // Set the button's state
            $button.data('state', (isChecked) ? "on" : "off");

            // Set the button's icon
            $button.find('.state-icon').removeClass().addClass('state-icon ' + settings[$button.data('state')].icon);

            // Update the button's color
            if (isChecked) {
                $button.removeClass('btn-default').addClass('btn-' + color + ' active');
            } else {
                $button.removeClass('btn-' + color + ' active').addClass('btn-default');
            }
        }

        // Initialization
        function init() {
            updateDisplay();

            // Inject the icon if applicable
            if ($button.find('.state-icon').length == 0) {
                $button.prepend('<i class="state-icon ' + settings[$button.data('state')].icon + '"></i> ');
            }
        }

        init();
    });

    var tableRows = 0;

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

    function contextualClassForType(messageType) {
        switch(messageType) {
            case "DEBUG":
            case "SUPPRESS":
                return "success";
            case "INFO":
                return "info";
            case "WARNING":
                return "warning";
            case "FATAL":
            case "CRITICAL":
                return "danger";
        }
    }

    function addRowToTable(index, message) {
        var messageArray = sanitizedMessageArray(index, message);
        var row = $("<tr/>");

        $.each(messageArray, function(index, column){
            var cell = $("<td>" + column + "</td>");

            if (index == 1) {
                // add the message type as a class to this cell
                cell.addClass(contextualClassForType(messageArray[1]));
            }

            row.append(cell);
        });

        // add the message type as data to this row
        row.attr("data-message-type", messageArray[1]);

        // append the row to the table
        $('table tbody').append(row);

        ++tableRows;
    }

    // when we get a new log entry, sanitize it and add it to the table
    Developer.newLogLine.connect(function(index, message){
        if (index >= tableRows) {
            addRowToTable(index, message);
        }
    });

    // enumerate the current log entries and set them up for DataTables
    $.each(Developer.log, function(index, message) {
        addRowToTable(index, message);
    });

    // change the column filter if the user asks for verbose debug
    $('#verbose-debug-checkbox').change(function(){
        if (this.checked) {
            // hide the debug output in the table
            $('tr[data-message-type="DEBUG"]').addClass('hidden-debug');
        } else {
            // show the debug output in the table
            $('tr[data-message-type="DEBUG"]').removeClass('hidden-debug');
        }
    });

    // fire the change event on the verbose debug checkbox to start with the right value
    $('#verbose-debug-checkbox').change();

    // handle filtering of table rows on input change
    $('#filter-search').on('input', function(){
        var filter = $(this).val().toLowerCase();
        $('tbody tr').each(function(){
            // decide to hide or show the row if it matches the filter
            if (filter && $(this).text().toLowerCase().indexOf(filter) == -1) {
                $(this).addClass('filtered');
            } else {
                $(this).removeClass('filtered');
            }
        });
    });
})
