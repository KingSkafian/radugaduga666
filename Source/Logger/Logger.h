#pragma once
#include <mutex>
#include <iostream>

struct logger_t
{
    bool success = true;
    mutex log_mutex;

    void set_color(int color_code)
    {
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!console) return;
        SetConsoleTextAttribute(console, color_code);
    }

    void reset_color()
    {
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!console) return;
        SetConsoleTextAttribute(console, 7);
    }

    template<typename... Args>
    void print(Args&&... args)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        cout << "[";
        set_color(13);
        cout << ">";
        set_color(7);
        cout << "] ";
        (cout << ... << args);
        cout << endl;
        reset_color();
    }

    void print_hex(const string& text, uint64_t hex_value)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        cout << "[";
        set_color(13);
        cout << ">";
        set_color(7);
        cout << "] ";
        cout << text;
        set_color(14);
        cout << "0x" << hex << hex_value << dec;
        cout << endl;
        reset_color();
    }

    void print_dec(const string& text, uint32_t dec_value)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        cout << "[";
        set_color(13);
        cout << ">";
        set_color(7);
        cout << "] ";
        cout << text;
        set_color(14);
        cout << dec_value;
        cout << endl;
        reset_color();
    }

    template<typename... Args>
    void error(Args&&... args)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(12);
        cout << "[!] ";
        set_color(7);
        (cout << ... << args);
        cout << endl;
        reset_color();
    }

    void sleep(int milliseconds)
    {
        Sleep(milliseconds);
    }

    void beep(int frequency, int duration)
    {
        Beep(frequency, duration);
    }

    void line(int length = 33)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        for (int i = 0; i < length; i++) cout << '-';
        cout << endl; reset_color();
    }

    void print_formula(const string& formula)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7); cout << "["; set_color(13); cout << ">"; set_color(7); cout << "] ";
        set_color(14); cout << formula << endl; reset_color();
    }

    void found_offset(const string& name, uint64_t offset)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        cout << "["; set_color(13); cout << ">"; set_color(7); cout << "] ";
        cout << name << ": ";
        set_color(14);
        cout << "0x" << hex << offset << dec;
        cout << endl;
        reset_color();
    }

    void found_formula(const string& name, const string& formula)
    {
        lock_guard<mutex> lock(log_mutex);
        set_color(7);
        cout << "["; set_color(13); cout << ">"; set_color(7); cout << "] ";
        cout << name << ": ";
        set_color(14);
        cout << formula << endl;
        reset_color();
    }

    template<typename... Args>
    string format(Args&&... args)
    {
        stringstream ss;
        (ss << ... << args);
        return ss.str();
    }
};

inline logger_t* Logger = new logger_t();