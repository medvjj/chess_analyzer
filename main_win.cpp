#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Preferences.H>

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Элементы главного окна
Fl_Input* engine_input = nullptr;
Fl_Input* fen_input = nullptr;
Fl_Input* move_input = nullptr;
Fl_Text_Display* output_display = nullptr;
Fl_Text_Buffer* output_buffer = nullptr;
Fl_Box* status_box = nullptr;
Fl_Box* evaluation_box = nullptr;
Fl_Spinner* font_size_spinner = nullptr;

// Постоянные пользовательские настройки
Fl_Preferences preferences(
    Fl_Preferences::USER,
    "MyChessTools",
    "StockfishAnalyzer"
);

// Параметры Stockfish: по одному на строку, формат Имя=Значение
std::string engine_options;

// Состояние процесса Stockfish
std::atomic<bool> process_running(false);
std::atomic<bool> engine_ready(false);
std::atomic<bool> search_running(false);
std::atomic<bool> start_when_ready(false);

std::thread engine_thread;
std::mutex pipe_mutex;
std::mutex pending_mutex;

HANDLE engine_stdin = nullptr;

// Отложенное действие после получения bestmove.
enum class PendingAction {
    None,
    ApplyMove,
    StartPosition
};

PendingAction pending_action = PendingAction::None;
std::string pending_action_fen;
std::string pending_action_move;
std::string pending_fen;

// Сообщения для главного потока FLTK
struct UiMessage {
    std::string text;
    bool is_fen = false;
    bool is_evaluation = false;
};

void load_settings()
{
    char* value = nullptr;

    preferences.get("engine_path", value, "");
    engine_input->value(value);
    std::free(value);

    value = nullptr;
    preferences.get("engine_options", value, "");
    engine_options = value;
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
        status_box->label("FEN обновлена");
    } else if (message->is_evaluation) {
        evaluation_box->label(message->text.c_str());
    } else {
        output_buffer->append(message->text.c_str());
        output_display->scroll(output_buffer->length(), 0);
    }

    delete message;
}

void set_status_callback(void* data)
{
    std::string* text = static_cast<std::string*>(data);
    status_box->label(text->c_str());
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

// Извлекает оценку из строки UCI "info ... score cp N" или "score mate N".
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
        text << "Оценка (сторона хода): ";

        if (score_type == "cp") {
            text << std::showpos << std::fixed << std::setprecision(2)
                 << (score_value / 100.0);
        } else if (score_type == "mate") {
            if (score_value > 0) {
                text << "мат через " << score_value;
            } else if (score_value < 0) {
                text << "получает мат через " << -score_value;
            } else {
                text << "мат";
            }
        } else {
            return;
        }

        post_evaluation(text.str());
        return;
    }
}

// Отправка команды Stockfish
bool send_command(const std::string& command)
{
    std::lock_guard<std::mutex> lock(pipe_mutex);

    if (engine_stdin == nullptr) {
        return false;
    }

    const std::string line = command + "\n";
    DWORD written = 0;

    return WriteFile(
        engine_stdin,
        line.c_str(),
        static_cast<DWORD>(line.size()),
        &written,
        nullptr
    ) != 0;
}

void start_search(const std::string& fen)
{
    if (!engine_ready) {
        post_status("Stockfish ещё запускается");
        return;
    }

    if (fen.empty()) {
        post_status("Введите позицию FEN");
        return;
    }

    if (!send_command("position fen " + fen)) {
        post_status("Не удалось отправить позицию Stockfish");
        return;
    }

    if (!send_command("go infinite")) {
        post_status("Не удалось запустить анализ");
        return;
    }

    search_running = true;
    post_status("Stockfish анализирует позицию");
}

void apply_move_and_start_search(
    const std::string& fen,
    const std::string& move)
{
    if (!engine_ready) {
        post_status("Stockfish ещё запускается");
        return;
    }

    if (move.empty()) {
        start_search(fen);
        return;
    }

    const std::string position_command =
        "position fen " + fen + " moves " + move;

    if (!send_command(position_command)) {
        post_status("Не удалось отправить позицию с ходом");
        return;
    }

    // Stockfish выведет новую позицию, включая строку "Fen: ...".
    if (!send_command("d")) {
        post_status("Не удалось запросить новую FEN");
        return;
    }

    if (!send_command("go infinite")) {
        post_status("Не удалось запустить анализ новой позиции");
        return;
    }

    search_running = true;
    post_status("Ход применён; анализируется новая позиция");
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

// Обрабатывает ответ bestmove и выполняет отложенное действие, если оно есть.
void handle_bestmove()
{
    PendingAction action = PendingAction::None;
    std::string fen;
    std::string move;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);

        search_running = false;

        action = pending_action;
        fen = pending_action_fen;
        move = pending_action_move;

        pending_action = PendingAction::None;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    if (action == PendingAction::ApplyMove) {
        apply_move_and_start_search(fen, move);
    } else if (action == PendingAction::StartPosition) {
        start_search(fen);
    } else {
        post_status("Расчёт завершён");
    }
}

void run_stockfish(std::string engine_path)
{
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;

    if (!CreatePipe(
            &child_stdin_read,
            &parent_stdin_write,
            &security_attributes,
            0) ||
        !CreatePipe(
            &parent_stdout_read,
            &child_stdout_write,
            &security_attributes,
            0)) {
        if (child_stdin_read) CloseHandle(child_stdin_read);
        if (parent_stdin_write) CloseHandle(parent_stdin_write);
        if (parent_stdout_read) CloseHandle(parent_stdout_read);
        if (child_stdout_write) CloseHandle(child_stdout_write);

        post_output("Не удалось создать каналы связи со Stockfish.\n");
        post_status("Ошибка запуска");
        process_running = false;
        return;
    }

    SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0);

    const int wide_length = MultiByteToWideChar(
        CP_UTF8,
        0,
        engine_path.c_str(),
        -1,
        nullptr,
        0
    );

    if (wide_length == 0) {
        CloseHandle(child_stdin_read);
        CloseHandle(parent_stdin_write);
        CloseHandle(parent_stdout_read);
        CloseHandle(child_stdout_write);

        post_output("Не удалось преобразовать путь к Stockfish.\n");
        post_status("Ошибка пути");
        process_running = false;
        return;
    }

    std::vector<wchar_t> wide_path(static_cast<std::size_t>(wide_length));

    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            engine_path.c_str(),
            -1,
            wide_path.data(),
            wide_length) == 0) {
        CloseHandle(child_stdin_read);
        CloseHandle(parent_stdin_write);
        CloseHandle(parent_stdout_read);
        CloseHandle(child_stdout_write);

        post_output("Не удалось преобразовать путь к Stockfish.\n");
        post_status("Ошибка пути");
        process_running = false;
        return;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_stdin_read;
    startup_info.hStdOutput = child_stdout_write;
    startup_info.hStdError = child_stdout_write;

    PROCESS_INFORMATION process_info{};

    std::wstring command_line =
        L"\"" + std::wstring(wide_path.data()) + L"\"";

    const BOOL created = CreateProcessW(
        wide_path.data(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup_info,
        &process_info
    );

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);

    if (!created) {
        CloseHandle(parent_stdin_write);
        CloseHandle(parent_stdout_read);

        post_output(
            "Не удалось запустить Stockfish. "
            "Проверьте путь к исполняемому файлу.\n"
        );
        post_status("Ошибка запуска");
        process_running = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pipe_mutex);
        engine_stdin = parent_stdin_write;
    }

    post_output("Stockfish запущен.\n");
    send_command("uci");

    std::string pending;
    char buffer[1024];
    DWORD bytes_read = 0;

    while (ReadFile(
               parent_stdout_read,
               buffer,
               sizeof(buffer) - 1,
               &bytes_read,
               nullptr) &&
           bytes_read > 0) {
        buffer[bytes_read] = '\0';
        pending.append(buffer, bytes_read);

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
                post_status("Stockfish готов");

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

    {
        std::lock_guard<std::mutex> lock(pipe_mutex);

        if (engine_stdin != nullptr) {
            CloseHandle(engine_stdin);
            engine_stdin = nullptr;
        }
    }

    CloseHandle(parent_stdout_read);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    engine_ready = false;
    process_running = false;
    search_running = false;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_action = PendingAction::None;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    post_status("Stockfish остановлен");
}

void browse_engine(Fl_Widget*, void*)
{
    Fl_Native_File_Chooser chooser;
    chooser.title("Выберите Stockfish");
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter("Программы Windows (*.exe)\t*.exe");

    if (chooser.show() == 0 && chooser.filename() != nullptr) {
        engine_input->value(chooser.filename());
        save_settings();
    }
}

void open_settings(Fl_Widget*, void*)
{
    Fl_Window settings_window(430, 270, "Параметры Stockfish");

    Fl_Box explanation(
        15, 15, 400, 50,
        "Укажите по одному параметру на строку\n"
        "в формате Имя=Значение, например:"
    );
    explanation.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Multiline_Input options_input(15, 70, 400, 145);
    options_input.value(engine_options.c_str());

    Fl_Button save_button(245, 225, 80, 30, "Сохранить");
    Fl_Button cancel_button(335, 225, 80, 30, "Отмена");

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
        status_box->label("Введите позицию FEN");
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
            post_status("Остановка поиска перед запуском новой позиции...");
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
        status_box->label("Укажите путь к Stockfish");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_fen = fen;
        pending_action = PendingAction::None;
        pending_action_fen.clear();
        pending_action_move.clear();
    }

    start_when_ready = true;
    process_running = true;
    engine_ready = false;
    search_running = false;

    output_buffer->text("");
    evaluation_box->label("Оценка: —");
    status_box->label("Запуск Stockfish...");

    engine_thread = std::thread(run_stockfish, engine_path);
}

void stop_button_callback(Fl_Widget*, void*)
{
    if (!process_running) {
        status_box->label("Stockfish не запущен");
        return;
    }

    if (search_running) {
        send_command("stop");
        status_box->label("Остановка поиска...");
    } else {
        status_box->label("Поиск не выполняется");
    }
}

void make_move_callback(Fl_Widget*, void*)
{
    if (!process_running || !engine_ready) {
        status_box->label("Сначала запустите Stockfish кнопкой «Пуск»");
        return;
    }

    const std::string fen = fen_input->value();
    const std::string move = move_input->value();

    if (fen.empty()) {
        status_box->label("Введите позицию FEN");
        return;
    }

    if (move.size() != 4 && move.size() != 5) {
        status_box->label("Введите ход, например e2e4 или e7e8q");
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
        status_box->label(
            "Остановка поиска перед применением вашего хода..."
        );
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
    Fl_Window window(760, 590, "Stockfish — анализ позиции");

    Fl_Box engine_label(15, 15, 115, 30, "Путь к движку:");
    engine_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    engine_input = new Fl_Input(130, 15, 510, 30);

    Fl_Button browse_button(650, 15, 95, 30, "Обзор...");
    browse_button.callback(browse_engine);

    Fl_Box fen_label(15, 60, 115, 30, "Позиция FEN:");
    fen_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    fen_input = new Fl_Input(130, 60, 615, 30);
    fen_input->value(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    );

    Fl_Box move_label(15, 105, 115, 30, "Ход UCI:");
    move_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    move_input = new Fl_Input(130, 105, 170, 30);

    Fl_Button make_move_button(310, 105, 145, 30, "Сделать ход");
    make_move_button.callback(make_move_callback);

    Fl_Button settings_button(15, 150, 150, 35, "Параметры...");
    settings_button.callback(open_settings);

    Fl_Button start_button(180, 150, 120, 35, "Пуск");
    start_button.callback(start_button_callback);

    Fl_Button stop_button(315, 150, 120, 35, "Стоп");
    stop_button.callback(stop_button_callback);

    status_box = new Fl_Box(455, 150, 290, 35, "Готов");
    status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    evaluation_box = new Fl_Box(15, 195, 500, 25, "Оценка: —");
    evaluation_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Box font_label(540, 195, 55, 25, "Шрифт:");
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

    // Загружаем сохранённый путь и параметры после создания полей.
    load_settings();

    window.resizable(&window);
    window.callback(close_main_window);

    window.end();
    window.show();

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
