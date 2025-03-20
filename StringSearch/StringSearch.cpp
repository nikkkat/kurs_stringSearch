#include <windows.h>
#include <shlobj.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <deque>
#include <condition_variable>


// Глобальные переменные
HINSTANCE hInst;
HWND hEditSearchString, hEditResults, hButtonBrowse, hButtonSearch, hButtonUp, hButtonDown, hStaticStats;
std::wstring searchString, directoryPath;
std::mutex resultMutex;
std::mutex queueMutex;
std::condition_variable cv;
std::deque<std::wstring> taskQueue;
std::atomic<bool> stopThreads = false;
const size_t THREAD_POOL_SIZE = 10;
std::vector<std::pair<std::wstring, size_t>> foundFiles;
std::vector<HWND> dynamicControls;
std::vector<std::pair<HWND, HWND>> tableRows;
const int MAX_ROWS = 100;

int currentRow = 0;
const int VISIBLE_ROWS = 28;


int filesChecked = 0;  // Количество проверенных файлов
int matchesFound = 0;  // Количество найденных совпадений

// Прототипы функций
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void WorkerThread();
void SearchInFile(const std::wstring& filePath);
void BrowseForFolder(HWND hwnd);
void UpdateResultsTable();
void StartSearch();
void StopThreadPool();
void ClearDynamicControls();
void CreateEmptyResultsTable(HWND hWnd);

void UpdateStatistics();

// Оптимизированная версия функции поиска
void SearchInFile(const std::wstring& filePath) {
    std::wifstream file(filePath);
    if (!file.is_open()) return;

    file.imbue(std::locale("ru_RU.UTF-8")); // Устанавливаем локаль для корректного чтения русских символов

    std::wstring line;
    size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        if (line.find(searchString) != std::wstring::npos) {
            std::lock_guard<std::mutex> lock(resultMutex);
            foundFiles.emplace_back(filePath, lineNumber);
            matchesFound++;  // Увеличиваем количество найденных совпадений
            break; // Если строка найдена, прерываем поиск в файле
        }
    }
    
    filesChecked++;
    UpdateStatistics();

}

// Оптимизированный поток для обработки задач
void WorkerThread() {
    while (true) {
        std::wstring task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [] { return !taskQueue.empty() || stopThreads; });

            if (stopThreads && taskQueue.empty()) break;

            task = taskQueue.front();
            taskQueue.pop_front();
        }

        WIN32_FIND_DATA findData;
        HANDLE hFind = FindFirstFile((task + L"\\*").c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring fileOrDir = findData.cFileName;
                if (fileOrDir == L"." || fileOrDir == L"..") continue;

                std::wstring fullPath = task + L"\\" + fileOrDir;

                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    taskQueue.push_back(fullPath);
                    cv.notify_one();
                }
                else {
                    SearchInFile(fullPath); // Оптимизация: обработка файла напрямую
                }
            } while (FindNextFile(hFind, &findData));
            FindClose(hFind);
        }
    }
}

// Запуск поиска с оптимизированной синхронизацией
void StartSearch() {
    foundFiles.clear();
    filesChecked = 0;
    matchesFound = 0;

    ClearDynamicControls();

    stopThreads = false;
    std::vector<std::thread> threadPool;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push_back(directoryPath);
    }
    cv.notify_one();

    // Создаем пул потоков для параллельной обработки
    for (size_t i = 0; i < THREAD_POOL_SIZE; ++i) {
        threadPool.emplace_back(WorkerThread);
    }

    // Ожидаем завершения всех потоков
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopThreads = true;
    }
    cv.notify_all();

    for (auto& thread : threadPool) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Обновляем результаты
    UpdateResultsTable();
    UpdateStatistics();
}

void ClearDynamicControls() {
    for (HWND hwnd : dynamicControls) {
        DestroyWindow(hwnd);
    }
    dynamicControls.clear();
}

// Обновление результатов в таблице
void UpdateResultsTable() {
    int row = 0;
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        
        for (int i = currentRow; i < currentRow + VISIBLE_ROWS && i < foundFiles.size(); ++i) {
            const auto& entry = foundFiles[i];
            const std::wstring& file = entry.first;
            const std::wstring line = std::to_wstring(entry.second);

            SetWindowText(tableRows[row].first, file.c_str());
            SetWindowText(tableRows[row].second, line.c_str());
            ++row;
        }
    }

    // Очищаем оставшиеся строки, если найдено меньше результатов
    for (; row < VISIBLE_ROWS; ++row) {
        SetWindowText(tableRows[row].first, L"");
        SetWindowText(tableRows[row].second, L"");
    }
}

void CreateEmptyResultsTable(HWND hWnd) {
    // Очистка предыдущих динамических элементов управления
    ClearDynamicControls();

    int xOffset = 10; // Начальные координаты для таблицы
    int yOffset = 110;
    int rowHeight = 25; // Высота строки
    int colWidth1 = 700; // Ширина первого столбца (для имени файла)
    int colWidth2 = 200; // Ширина второго столбца (для номера строки)

    // Создаем заголовки таблицы
    HWND hHeader1 = CreateWindow(L"STATIC", L"Файл", WS_VISIBLE | WS_CHILD | SS_CENTER,
        xOffset, yOffset, colWidth1, rowHeight,
        hWnd, nullptr, hInst, nullptr);
    dynamicControls.push_back(hHeader1);  // Сохраняем в список динамических элементов управления

    HWND hHeader2 = CreateWindow(L"STATIC", L"Строка", WS_VISIBLE | WS_CHILD | SS_CENTER,
        xOffset + colWidth1, yOffset, colWidth2, rowHeight,
        hWnd, nullptr, hInst, nullptr);
    dynamicControls.push_back(hHeader2);  // Сохраняем в список динамических элементов управления

    // Увеличиваем смещение по Y для размещения строк
    yOffset += rowHeight;

    // Создаем пустые строки таблицы (для отображения найденных файлов и строк)
    for (int i = 0; i < VISIBLE_ROWS; ++i) {
        // Первый столбец - поле для отображения пути файла
        HWND hFileEdit = CreateWindow(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY,
            xOffset, yOffset, colWidth1, rowHeight,
            hWnd, nullptr, hInst, nullptr);
        dynamicControls.push_back(hFileEdit);  // Добавляем в список

        // Второй столбец - поле для отображения номера строки
        HWND hLineEdit = CreateWindow(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY | ES_CENTER,
            xOffset + colWidth1, yOffset, colWidth2, rowHeight,
            hWnd, nullptr, hInst, nullptr);
        dynamicControls.push_back(hLineEdit);  // Добавляем в список

        // Сохраняем пары контролов для дальнейшей работы
        tableRows.push_back({ hFileEdit, hLineEdit });

        // Увеличиваем смещение по Y для следующей строки
        yOffset += rowHeight;
    }

    hButtonUp = CreateWindow(L"BUTTON", L"Вверх", WS_CHILD | WS_VISIBLE,
        950, 320, 75, 30, hWnd, (HMENU)3, hInst, nullptr);
    hButtonDown = CreateWindow(L"BUTTON", L"Вниз", WS_CHILD | WS_VISIBLE,
        1050, 320, 75, 30, hWnd, (HMENU)4, hInst, nullptr);

    // Статистика
    hStaticStats = CreateWindow(L"STATIC", L"",
        WS_VISIBLE | WS_CHILD, 400, 70, 500, 30, hWnd, nullptr, hInst, nullptr);

}

// Обновление статистики
void UpdateStatistics() {
    std::wstring stats;
    stats += L"Просмотрено файлов: " + std::to_wstring(filesChecked) + L"\n";
    stats += L"Найдено совпадений: " + std::to_wstring(matchesFound);

    SetWindowText(hStaticStats, stats.c_str());
}

// Обработчики кнопок
void OnButtonUpClick() {
    if (currentRow > 0) {
        --currentRow;
        UpdateResultsTable();
    }
}

void OnButtonDownClick() {
    if (currentRow + VISIBLE_ROWS < foundFiles.size()) {
        ++currentRow;
        UpdateResultsTable();
    }
}

// Выбор папки
void BrowseForFolder(HWND hwnd) {
    BROWSEINFO bi = { 0 };
    bi.lpszTitle = L"Выберите папку для поиска";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (pidl) {
        WCHAR path[MAX_PATH];
        if (SHGetPathFromIDList(pidl, path)) {
            directoryPath = path;
        }
        CoTaskMemFree(pidl);
    }
}

// Остановка пула потоков
void StopThreadPool() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopThreads = true;
    }
    cv.notify_all();
}

// Главная функция окна
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindow(L"STATIC", L"Строка поиска:", WS_VISIBLE | WS_CHILD, 10, 10, 120, 20, hWnd, nullptr, hInst, nullptr);
        hEditSearchString = CreateWindow(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 140, 10, 400, 20, hWnd, nullptr, hInst, nullptr);
        hButtonBrowse = CreateWindow(L"BUTTON", L"Выбрать папку", WS_VISIBLE | WS_CHILD, 10, 40, 120, 30, hWnd, (HMENU)1, hInst, nullptr);
        hButtonSearch = CreateWindow(L"BUTTON", L"Искать", WS_VISIBLE | WS_CHILD, 140, 40, 120, 30, hWnd, (HMENU)2, hInst, nullptr);
        CreateWindow(L"STATIC", L"Результаты:", WS_VISIBLE | WS_CHILD, 10, 80, 120, 20, hWnd, nullptr, hInst, nullptr);
        CreateEmptyResultsTable(hWnd);
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 1) { // Кнопка "Выбрать папку"
            BrowseForFolder(hWnd);
        }
        else if (LOWORD(wParam) == 2) { // Кнопка "Искать"
            int len = GetWindowTextLength(hEditSearchString);
            if (len > 0) {
                WCHAR* buffer = new WCHAR[len + 1];
                GetWindowText(hEditSearchString, buffer, len + 1);
                searchString = buffer;
                delete[] buffer;

                if (!directoryPath.empty()) {
                    std::thread searchThread([]() { StartSearch(); });
                    searchThread.detach();
                }
                else {
                    MessageBox(hWnd, L"Сначала выберите папку.", L"Ошибка", MB_OK | MB_ICONERROR);
                }
            }
            else {
                MessageBox(hWnd, L"Введите строку для поиска.", L"Ошибка", MB_OK | MB_ICONERROR);
            }
        }
        else if (LOWORD(wParam) == 3) {
            OnButtonUpClick();
        }
        else if (LOWORD(wParam) == 4) {
            OnButtonDownClick();
        }
        break;
    }
    case WM_DESTROY:
        StopThreadPool();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Точка входа
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SearchApp";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) return 0;

    HWND hWnd = CreateWindow(L"SearchApp", L"Поиск строки в файлах", WS_OVERLAPPEDWINDOW, 360, 100, 1200, 900, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
