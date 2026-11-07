g++ -std=c++17 -static main_win.cpp -o stockfish_gui.exe $(fltk-config --cxxflags --ldflags) -luuid
