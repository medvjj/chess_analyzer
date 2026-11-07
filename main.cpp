#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Preferences.H>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <clocale>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Main window widgets
Fl_Input* engine_input = nullptr;
Fl_Input* fen_input = nullptr;
Fl_Input* move_input = nullptr;
Fl_Text_Display* output_display = nullptr;
Fl_Text_Buffer* output_buffer = nullptr;
Fl_Box* status_box = nullptr;
Fl_Box* evaluation_box = nullptr;
Fl_Spinner* font_size_spinner = nullptr;

// Persistent user settings
Fl_Preferences preferences(
    Fl_Preferences::USER,
    "MyChessTools",
    "StockfishAnalyzer"
);

// Stockfish options: one per line, in Name=Value format
std::string engine_options;

// Stockfish process state
std::atomic<bool> process_running(false);
std::atomic<bool> engine_ready(false);
std::atomic<bool> search_running(false);
std::atomic<bool> start_when_ready(false);

std::thread engine_thread;
std::mutex pipe_mutex;
std::mutex pending_mutex;

int engine_stdin = -1;
pid_t engine_pid = -1;

// Action to perform after receiving bestmove
enum class PendingAction {
    NoAction,
    ApplyMove,
    StartPosition
};

PendingAction pending_action = PendingAction::NoAction;
std::string pending_action_fen;
std::string pending_action_move;
std::string pending_fen;

// Messages posted to the FLTK main thread
struct UiMessage {
    std::string text;
    bool is_fen = false;
    bool is_evaluation = false;
};

void load_settings()
{
    char* value = nullptr;

    preferences.get("engine_path", value, "");
    engine_input->value(value != nullptr ? value : "");
    std::free(value);

    value = nullptr;
    preferences.get("engine_options", value, "");
    engine_options = value != nullptr ? value : "";
    std::free(value);
}

void save_settings()
{
    if (engine_input != nullptr) {
        preferences.set("engine_path", engine_input->value());
    }

    preferences.set("engine_options", engine_options.c_str());
    preferences.flush();
}

void append_to_output(void* data)
{
    UiMessage* message = static_cast<UiMessage*>(data);

    if (message->is_fen) {
        fen_input->value(message->text.c_str());
        status_box->label("FEN updated");
    } else if (message->is_evaluation) {
        evaluation_box->copy_label(message->text.c_str());
    } else {
        output_buffer->append(message->text.c_str());
        output_display->scroll(output_buffer->length(), 0);
    }

    delete message;
}

void set_status_callback(void* data)
{
    std::string* text = static_cast<std::string*>(data);
    status_box->copy_label(text->c_str());
    delete text;
}

void set_font_size_callback(void* data)
{
    int* size = static_cast<int*>(data);
    output_display->textsize(*size);
    output_display->redraw();
    delete size;
}

void post_output(const std::string& text)
{
    Fl::awake(append_to_output, new UiMessage{text, false, false});
}

void post_fen(const std::string& fen)
{
    Fl::awake(append_to_output, new UiMessage{fen, true, false});
}

void post_evaluation(const std::string& evaluation)
{
    Fl::awake(append_to_output, new UiMessage{evaluation, false, true});
}

void post_status(const std::string& text)
{
    Fl::awake(set_status_callback, new std::string(text));
}

void post_font_size(int size)
{
    Fl::awake(set_font_size_callback, new int(size));
}

// Parse UCI "info ... score cp N" or "score mate N" lines.
void update_evaluation_from_line(const std::string& line)
{
    if (line.rfind("info ", 0) != 0) {
        return;
    }

    std::istringstream stream(line);
    std::string token;

    while (stream >> token) {
        if (token != "score") {
            continue;
        }

        std::string score_type;
        int score_value = 0;

        if (!(stream >> score_type >> score_value)) {
            return;
        }

        std::ostringstream text;
        text << "Evaluation (side to move): ";

        if (score_type == "cp") {
            text << std::showpos << std::fixed << std::setprecision(2)
                 << (score_value / 100.0);
        } else if (score_type == "mate") {
            if (score_value > 0) {
                text << "mate in " << score_value;
            } else if (score_value < 0) {
                text << "getting mated in " << -score_value;
            } else {
                text << "mate";
            }
        } else {
            return;
        }

        post_evaluation(text.str());
        return;
    }
}

// Send a command to Stockfish.
bool send_command(const std::string& command)
{
    std::lock_guard<std::mutex> lock(pipe_mutex);

    if (engine_stdin < 0) {
        return false;
    }

    const std::string line = command + "\n";
    std::size_t total_written = 0;

    while (total_written < line.size()) {
        const ssize_t written = write(
            engine_stdin,
            line.data() + total_written,
            line.size() - total_written
        );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (written == 0) {
            return false;
        }

        total_written += static_cast<std::size_t>(written);
    }

    return true;
}

void start_search(const std::string& fen)
{
    if (!engine_ready) {
        post_status("Stockfish is still starting");
        return;
    }

    if (fen.empty()) {
        post_status("Enter a FEN position");
        return;
    }

    if (!send_command("position fen " + fen)) {
        post_status("Could not send the position to Stockfish");
        return;
    }

    if (!send_command("go infinite")) {
        post_status("Could not start analysis");
        return;
    }

    search_running = true;
    post_status("Stockfish is analyzing the position");
}

void apply_move_and_start_search(
    const std::string& fen,
    const std::string& move)
{
    if (!engine_ready) {
        post_status("Stockfish is still starting");
        return;
    }

    if (move.empty()) {
        start_search(fen);
        return;
    }

    const std::string position_command =
        "position fen " + fen + " moves " + move;

    if (!send_command(position_command)) {
        post_status("Could not send the position with the move");
        return;
    }

    // Stockfish prints the resulting position, including a "Fen: ..." line.
    if (!send_command("d")) {
        post_status("Could not request the new FEN");
        return;
    }

    if (!send_command("go infinite")) {
        post_status("Could not start analysis of the new position");
        return;
    }

    search_running = true;
    post_status("Move applied; analyzing the new position");
}

void send_engine_options()
{
    std::size_t start = 0;

    while (start < engine_options.size()) {
        std::size_t end = engine_options.find('\n', start);

        if (end == std::string::npos) {
            end = engine_options.size();
        }

        std::string option = engine_options.substr(start, end - start);

        if (!option.empty() && option.back() == '\r') {
            option.pop_back();
        }

        const std::size_t equal = option.find('=');

        if (equal != std::string::npos) {
            const std::string name = option.substr(0, equal);
            const std::string value = option.substr(equal + 1);

            if (!name.empty()) {
                send_command("setoption name " + name + " value " + value);
            }
        }

        start = end + 1;
    }
}

// Handle bestmove and perform any pending action.
void handle_bestmove()
{
    PendingAction action = PendingAction::NoAction;
    std::string fen;
    std::string move;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);

        search_running = false;

        action = pending_action;
        fen = pending_action_fen;
        move = pending_action_move;

        pending_action = PendingAction::NoAction;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    if (action == PendingAction::ApplyMove) {
        apply_move_and_start_search(fen, move);
    } else if (action == PendingAction::StartPosition) {
        start_search(fen);
    } else {
        post_status("Analysis finished");
    }
}

void run_stockfish(std::string engine_path)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);

        post_output("Could not create communication pipes for Stockfish.\n");
        post_status("Startup error");
        process_running = false;
        return;
    }

    const pid_t pid = fork();

    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        post_output("Could not create the Stockfish process.\n");
        post_status("Startup error");
        process_running = false;
        return;
    }

    if (pid == 0) {
        // Child process: connect pipes to standard input and output.
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        execl(
            engine_path.c_str(),
            engine_path.c_str(),
            static_cast<char*>(nullptr)
        );

        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    {
        std::lock_guard<std::mutex> lock(pipe_mutex);
        engine_stdin = stdin_pipe[1];
        engine_pid = pid;
    }

    post_output("Stockfish process started.\n");
    send_command("uci");

    std::string pending;
    char buffer[1024];

    while (true) {
        const ssize_t bytes_read = read(
            stdout_pipe[0],
            buffer,
            sizeof(buffer)
        );

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (bytes_read == 0) {
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(bytes_read));

        std::size_t newline_position = 0;

        while ((newline_position = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline_position);
            pending.erase(0, newline_position + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            post_output(line + "\n");
            update_evaluation_from_line(line);

            if (line == "uciok") {
                send_engine_options();
                send_command("isready");
            } else if (line == "readyok") {
                engine_ready = true;
                post_status("Stockfish is ready");

                if (start_when_ready.exchange(false)) {
                    std::string fen;

                    {
                        std::lock_guard<std::mutex> lock(pending_mutex);
                        fen = pending_fen;
                    }

                    start_search(fen);
                }
            } else if (line.rfind("Fen: ", 0) == 0) {
                post_fen(line.substr(5));
            } else if (line.rfind("bestmove ", 0) == 0) {
                handle_bestmove();
            }
        }
    }

    close(stdout_pipe[0]);

    {
        std::lock_guard<std::mutex> lock(pipe_mutex);

        if (engine_stdin >= 0) {
            close(engine_stdin);
            engine_stdin = -1;
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    {
        std::lock_guard<std::mutex> lock(pipe_mutex);
        engine_pid = -1;
    }

    engine_ready = false;
    process_running = false;
    search_running = false;
    start_when_ready = false;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_action = PendingAction::NoAction;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    post_status("Stockfish stopped");
}

void browse_engine(Fl_Widget*, void*)
{
    Fl_File_Chooser chooser(
        ".",
        "*",
        Fl_File_Chooser::SINGLE,
        "Select Stockfish"
    );

    chooser.show();

    while (chooser.shown()) {
        Fl::wait();
    }

    if (chooser.value() != nullptr) {
        engine_input->value(chooser.value());
        save_settings();
    }
}

void open_settings(Fl_Widget*, void*)
{
    Fl_Window settings_window(430, 270, "Stockfish options");

    Fl_Box explanation(
        15, 15, 400, 50,
        "Enter one option per line\n"
        "in Name=Value format, for example:"
    );
    explanation.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Multiline_Input options_input(15, 70, 400, 145);
    options_input.value(engine_options.c_str());

    Fl_Button save_button(245, 225, 80, 30, "Save");
    Fl_Button cancel_button(335, 225, 80, 30, "Cancel");

    save_button.callback(
        [](Fl_Widget*, void* data) {
            Fl_Window* window = static_cast<Fl_Window*>(data);
            Fl_Multiline_Input* input =
                static_cast<Fl_Multiline_Input*>(window->child(1));

            if (input != nullptr) {
                engine_options = input->value();
                save_settings();
            }

            window->hide();
        },
        &settings_window
    );

    cancel_button.callback(
        [](Fl_Widget*, void* data) {
            static_cast<Fl_Window*>(data)->hide();
        },
        &settings_window
    );

    settings_window.set_modal();
    settings_window.end();
    settings_window.show();

    while (settings_window.shown()) {
        Fl::wait();
    }
}

void start_button_callback(Fl_Widget*, void*)
{
    const std::string fen = fen_input->value();

    if (fen.empty()) {
        status_box->label("Enter a FEN position");
        return;
    }

    if (process_running) {
        if (search_running) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                pending_action = PendingAction::StartPosition;
                pending_action_fen = fen;
                pending_action_move.clear();
            }

            send_command("stop");
            post_status("Stopping search before starting a new position...");
        } else {
            start_search(fen);
        }

        return;
    }

    if (engine_thread.joinable()) {
        engine_thread.join();
    }

    const std::string engine_path = engine_input->value();

    if (engine_path.empty()) {
        status_box->label("Select the Stockfish executable");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_fen = fen;
        pending_action = PendingAction::NoAction;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    start_when_ready = true;
    process_running = true;
    engine_ready = false;
    search_running = false;

    output_buffer->text("");
    evaluation_box->label("Evaluation: --");
    status_box->label("Starting Stockfish...");

    engine_thread = std::thread(run_stockfish, engine_path);
}

void stop_button_callback(Fl_Widget*, void*)
{
    if (!process_running) {
        status_box->label("Stockfish is not running");
        return;
    }

    if (search_running) {
        send_command("stop");
        status_box->label("Stopping analysis...");
    } else {
        status_box->label("No analysis is running");
    }
}

void make_move_callback(Fl_Widget*, void*)
{
    if (!process_running || !engine_ready) {
        status_box->label("Start Stockfish first using the Start button");
        return;
    }

    const std::string fen = fen_input->value();
    const std::string move = move_input->value();

    if (fen.empty()) {
        status_box->label("Enter a FEN position");
        return;
    }

    if (move.size() != 4 && move.size() != 5) {
        status_box->label("Enter a move, for example e2e4 or e7e8q");
        return;
    }

    bool must_stop_first = false;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);

        if (search_running) {
            pending_action = PendingAction::ApplyMove;
            pending_action_fen = fen;
            pending_action_move = move;
            must_stop_first = true;
        }
    }

    move_input->value("");

    if (must_stop_first) {
        send_command("stop");
        status_box->label("Stopping search before applying your move...");
    } else {
        apply_move_and_start_search(fen, move);
    }
}

void font_size_callback(Fl_Widget* widget, void*)
{
    Fl_Spinner* spinner = static_cast<Fl_Spinner*>(widget);
    const int size = static_cast<int>(spinner->value());
    post_font_size(size);
}

void close_main_window(Fl_Widget* widget, void*)
{
    save_settings();

    if (process_running) {
        send_command("quit");
    }

    if (engine_thread.joinable()) {
        engine_thread.join();
    }

    static_cast<Fl_Window*>(widget)->hide();
}

int main()
{
    std::setlocale(LC_ALL, "");

    std::signal(SIGPIPE, SIG_IGN);

    Fl_Window window(760, 590, "Stockfish — Position Analyzer");

    Fl_Box engine_label(15, 15, 115, 30, "Engine path:");
    engine_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    engine_input = new Fl_Input(130, 15, 510, 30);

    Fl_Button browse_button(650, 15, 95, 30, "Browse...");
    browse_button.callback(browse_engine);

    Fl_Box fen_label(15, 60, 115, 30, "FEN position:");
    fen_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    fen_input = new Fl_Input(130, 60, 615, 30);
    fen_input->value(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    );

    Fl_Box move_label(15, 105, 115, 30, "UCI move:");
    move_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    move_input = new Fl_Input(130, 105, 170, 30);

    Fl_Button make_move_button(310, 105, 145, 30, "Make Move");
    make_move_button.callback(make_move_callback);

    Fl_Button settings_button(15, 150, 150, 35, "Options...");
    settings_button.callback(open_settings);

    Fl_Button start_button(180, 150, 120, 35, "Start");
    start_button.callback(start_button_callback);

    Fl_Button stop_button(315, 150, 120, 35, "Stop");
    stop_button.callback(stop_button_callback);

    status_box = new Fl_Box(455, 150, 290, 35, "Ready");
    status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    evaluation_box = new Fl_Box(15, 195, 500, 25, "Evaluation: --");
    evaluation_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Box font_label(540, 195, 55, 25, "Font:");
    font_label.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    font_size_spinner = new Fl_Spinner(600, 195, 75, 25);
    font_size_spinner->range(8, 32);
    font_size_spinner->step(1);
    font_size_spinner->value(14);
    font_size_spinner->callback(font_size_callback);

    output_buffer = new Fl_Text_Buffer();

    output_display = new Fl_Text_Display(15, 230, 730, 345);
    output_display->buffer(output_buffer);
    output_display->textsize(14);

    // Load saved path and options after creating the input fields.
    load_settings();

    window.resizable(&window);
    window.callback(close_main_window);

    window.end();
    window.show();

    if (Fl::lock() != 0) {
        status_box->label("Could not initialize FLTK thread support");
    }

    const int result = Fl::run();

    save_settings();

    if (process_running) {
        send_command("quit");
    }

    if (engine_thread.joinable()) {
        engine_thread.join();
    }

    delete output_buffer;

    return result;
}
