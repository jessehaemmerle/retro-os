import React, { useState, useEffect, useRef } from 'react';
import './App.css';
import axios from 'axios';

const BACKEND_URL = process.env.REACT_APP_BACKEND_URL;
const API = `${BACKEND_URL}/api`;

// Window Management Component
const Window = ({ 
  id, 
  title, 
  children, 
  onClose, 
  onMinimize, 
  onMaximize, 
  isMaximized = false,
  initialPosition = { x: 100, y: 100 },
  initialSize = { width: 600, height: 400 },
  zIndex = 1
}) => {
  const [position, setPosition] = useState(initialPosition);
  const [size, setSize] = useState(initialSize);
  const [isDragging, setIsDragging] = useState(false);
  const [isResizing, setIsResizing] = useState(false);
  const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });
  const windowRef = useRef(null);

  const handleMouseDown = (e) => {
    if (e.target.classList.contains('window-title-bar')) {
      setIsDragging(true);
      const rect = windowRef.current.getBoundingClientRect();
      setDragOffset({
        x: e.clientX - rect.left,
        y: e.clientY - rect.top
      });
    }
  };

  const handleMouseMove = (e) => {
    if (isDragging && !isMaximized) {
      setPosition({
        x: e.clientX - dragOffset.x,
        y: Math.max(0, e.clientY - dragOffset.y)
      });
    }
  };

  const handleMouseUp = () => {
    setIsDragging(false);
    setIsResizing(false);
  };

  useEffect(() => {
    if (isDragging || isResizing) {
      document.addEventListener('mousemove', handleMouseMove);
      document.addEventListener('mouseup', handleMouseUp);
      return () => {
        document.removeEventListener('mousemove', handleMouseMove);
        document.removeEventListener('mouseup', handleMouseUp);
      };
    }
  }, [isDragging, isResizing, dragOffset]);

  const windowStyle = isMaximized 
    ? { top: 0, left: 0, width: '100%', height: 'calc(100vh - 40px)', zIndex }
    : { 
        top: position.y, 
        left: position.x, 
        width: size.width, 
        height: size.height, 
        zIndex 
      };

  return (
    <div 
      ref={windowRef}
      className="window"
      style={windowStyle}
      onMouseDown={handleMouseDown}
    >
      <div className="window-title-bar">
        <span className="window-title">{title}</span>
        <div className="window-controls">
          <button className="window-btn minimize-btn" onClick={() => onMinimize(id)}>_</button>
          <button className="window-btn maximize-btn" onClick={() => onMaximize(id)}>□</button>
          <button className="window-btn close-btn" onClick={() => onClose(id)}>×</button>
        </div>
      </div>
      <div className="window-content">
        {children}
      </div>
    </div>
  );
};

// Calculator App
const Calculator = () => {
  const [display, setDisplay] = useState('0');
  const [operation, setOperation] = useState(null);
  const [prevValue, setPrevValue] = useState(null);
  const [waitingForNewValue, setWaitingForNewValue] = useState(false);

  const inputNumber = (num) => {
    if (waitingForNewValue) {
      setDisplay(String(num));
      setWaitingForNewValue(false);
    } else {
      setDisplay(display === '0' ? String(num) : display + num);
    }
  };

  const inputOperation = (nextOperation) => {
    const inputValue = parseFloat(display);

    if (prevValue === null) {
      setPrevValue(inputValue);
    } else if (operation) {
      const currentValue = prevValue || 0;
      const newValue = calculate(currentValue, inputValue, operation);
      
      setDisplay(String(newValue));
      setPrevValue(newValue);
    }

    setWaitingForNewValue(true);
    setOperation(nextOperation);
  };

  const calculate = (firstValue, secondValue, operation) => {
    switch (operation) {
      case '+': return firstValue + secondValue;
      case '-': return firstValue - secondValue;
      case '*': return firstValue * secondValue;
      case '/': return firstValue / secondValue;
      case '=': return secondValue;
      default: return secondValue;
    }
  };

  const performCalculation = () => {
    const inputValue = parseFloat(display);

    if (prevValue !== null && operation) {
      const newValue = calculate(prevValue, inputValue, operation);
      setDisplay(String(newValue));
      setPrevValue(null);
      setOperation(null);
      setWaitingForNewValue(true);
    }
  };

  const allClear = () => {
    setDisplay('0');
    setOperation(null);
    setPrevValue(null);
    setWaitingForNewValue(false);
  };

  return (
    <div className="calculator">
      <div className="calc-display">{display}</div>
      <div className="calc-buttons">
        <button onClick={allClear} className="calc-btn calc-btn-wide">AC</button>
        <button onClick={() => inputOperation('/')} className="calc-btn calc-btn-operator">÷</button>
        
        <button onClick={() => inputNumber(7)} className="calc-btn">7</button>
        <button onClick={() => inputNumber(8)} className="calc-btn">8</button>
        <button onClick={() => inputNumber(9)} className="calc-btn">9</button>
        <button onClick={() => inputOperation('*')} className="calc-btn calc-btn-operator">×</button>
        
        <button onClick={() => inputNumber(4)} className="calc-btn">4</button>
        <button onClick={() => inputNumber(5)} className="calc-btn">5</button>
        <button onClick={() => inputNumber(6)} className="calc-btn">6</button>
        <button onClick={() => inputOperation('-')} className="calc-btn calc-btn-operator">-</button>
        
        <button onClick={() => inputNumber(1)} className="calc-btn">1</button>
        <button onClick={() => inputNumber(2)} className="calc-btn">2</button>
        <button onClick={() => inputNumber(3)} className="calc-btn">3</button>
        <button onClick={() => inputOperation('+')} className="calc-btn calc-btn-operator">+</button>
        
        <button onClick={() => inputNumber(0)} className="calc-btn calc-btn-wide">0</button>
        <button onClick={performCalculation} className="calc-btn calc-btn-operator">=</button>
      </div>
    </div>
  );
};

// Text Editor App
const TextEditor = ({ user, onSave }) => {
  const [content, setContent] = useState('');
  const [fileName, setFileName] = useState('Untitled.txt');
  const [isSaved, setIsSaved] = useState(true);

  const handleContentChange = (e) => {
    setContent(e.target.value);
    setIsSaved(false);
  };

  const saveFile = async () => {
    try {
      await axios.post(`${API}/files/${user.id}`, {
        name: fileName,
        type: 'file',
        content: content,
        path: `/Desktop/${fileName}`
      });
      setIsSaved(true);
      onSave && onSave();
    } catch (error) {
      console.error('Error saving file:', error);
    }
  };

  return (
    <div className="text-editor">
      <div className="text-editor-toolbar">
        <input 
          type="text" 
          value={fileName} 
          onChange={(e) => setFileName(e.target.value)}
          className="filename-input"
        />
        <button onClick={saveFile} className="save-btn" disabled={isSaved}>
          {isSaved ? 'Saved' : 'Save'}
        </button>
      </div>
      <textarea
        value={content}
        onChange={handleContentChange}
        className="text-editor-content"
        placeholder="Start typing..."
      />
    </div>
  );
};

// File Browser App
const FileBrowser = ({ user, onRefresh }) => {
  const [files, setFiles] = useState([]);
  const [currentPath, setCurrentPath] = useState('/');
  const [loading, setLoading] = useState(false);

  const loadFiles = async (path = '/') => {
    setLoading(true);
    try {
      const response = await axios.get(`${API}/files/${user.id}?path=${path}`);
      setFiles(response.data);
      setCurrentPath(path);
    } catch (error) {
      console.error('Error loading files:', error);
    }
    setLoading(false);
  };

  useEffect(() => {
    if (user) {
      loadFiles();
    }
  }, [user]);

  const createFolder = async () => {
    const folderName = prompt('Enter folder name:');
    if (folderName) {
      try {
        await axios.post(`${API}/files/${user.id}`, {
          name: folderName,
          type: 'folder',
          path: `${currentPath}${folderName}`
        });
        loadFiles(currentPath);
      } catch (error) {
        console.error('Error creating folder:', error);
      }
    }
  };

  const deleteFile = async (fileId) => {
    if (window.confirm('Are you sure you want to delete this file?')) {
      try {
        await axios.delete(`${API}/files/${user.id}/${fileId}`);
        loadFiles(currentPath);
      } catch (error) {
        console.error('Error deleting file:', error);
      }
    }
  };

  return (
    <div className="file-browser">
      <div className="file-browser-toolbar">
        <div className="path-display">Path: {currentPath}</div>
        <button onClick={createFolder} className="new-folder-btn">New Folder</button>
      </div>
      <div className="file-list">
        {loading ? (
          <div>Loading...</div>
        ) : (
          files.map(file => (
            <div key={file.id} className="file-item">
              <div className="file-icon">
                {file.type === 'folder' ? '📁' : '📄'}
              </div>
              <div className="file-info">
                <div className="file-name">{file.name}</div>
                <div className="file-details">
                  {file.type} - {file.size} bytes
                </div>
              </div>
              <button 
                onClick={() => deleteFile(file.id)} 
                className="delete-btn"
              >
                🗑️
              </button>
            </div>
          ))
        )}
      </div>
    </div>
  );
};

// Web Browser App
const WebBrowser = () => {
  const [url, setUrl] = useState('https://www.google.com');
  const [currentUrl, setCurrentUrl] = useState('https://www.google.com');

  const navigate = () => {
    setCurrentUrl(url);
  };

  const handleKeyPress = (e) => {
    if (e.key === 'Enter') {
      navigate();
    }
  };

  return (
    <div className="web-browser">
      <div className="browser-toolbar">
        <input 
          type="text" 
          value={url} 
          onChange={(e) => setUrl(e.target.value)}
          onKeyPress={handleKeyPress}
          className="url-input"
          placeholder="Enter URL..."
        />
        <button onClick={navigate} className="go-btn">Go</button>
      </div>
      <iframe 
        src={currentUrl} 
        className="browser-frame"
        title="Web Browser"
      />
    </div>
  );
};

// Games Launcher App
const GamesLauncher = ({ onOpenGame }) => {
  const games = [
    { name: 'Solitaire', url: 'https://www.solitr.com/' },
    { name: 'Minesweeper', url: 'https://minesweeper.online/' },
    { name: 'Tetris', url: 'https://tetris.com/play-tetris' },
    { name: 'Snake', url: 'https://playsnake.org/' },
    { name: 'Pac-Man', url: 'https://pacman.live/' }
  ];

  return (
    <div className="games-launcher">
      <h3>Games</h3>
      <div className="games-grid">
        {games.map((game, index) => (
          <div 
            key={index} 
            className="game-tile"
            onClick={() => onOpenGame(game)}
          >
            <div className="game-icon">🎮</div>
            <div className="game-name">{game.name}</div>
          </div>
        ))}
      </div>
    </div>
  );
};

// Game Window Component
const GameWindow = ({ game }) => (
  <iframe 
    src={game.url} 
    className="game-frame"
    title={game.name}
  />
);

// Login Component
const Login = ({ onLogin, onRegister }) => {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [isRegistering, setIsRegistering] = useState(false);

  const handleSubmit = async (e) => {
    e.preventDefault();
    const endpoint = isRegistering ? 'register' : 'login';
    
    try {
      const response = await axios.post(`${API}/${endpoint}`, {
        username,
        password
      });
      
      if (isRegistering) {
        alert('Registration successful! Please log in.');
        setIsRegistering(false);
      } else {
        onLogin(response.data);
      }
    } catch (error) {
      alert(`${isRegistering ? 'Registration' : 'Login'} failed: ${error.response?.data?.detail || error.message}`);
    }
  };

  return (
    <div className="login-screen">
      <div className="login-box">
        <h2>{isRegistering ? 'Register' : 'Login'} to RetroOS</h2>
        <form onSubmit={handleSubmit}>
          <input
            type="text"
            placeholder="Username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            required
          />
          <input
            type="password"
            placeholder="Password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            required
          />
          <button type="submit">
            {isRegistering ? 'Register' : 'Login'}
          </button>
        </form>
        <button 
          onClick={() => setIsRegistering(!isRegistering)}
          className="switch-mode-btn"
        >
          {isRegistering ? 'Already have an account? Login' : 'Need an account? Register'}
        </button>
      </div>
    </div>
  );
};

// Main Desktop Component
const Desktop = ({ user, onLogout }) => {
  const [windows, setWindows] = useState([]);
  const [nextZIndex, setNextZIndex] = useState(1);
  const [currentTime, setCurrentTime] = useState(new Date());

  useEffect(() => {
    const timer = setInterval(() => {
      setCurrentTime(new Date());
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  const openWindow = (type, title, props = {}) => {
    const newWindow = {
      id: Date.now(),
      type,
      title,
      zIndex: nextZIndex,
      isMaximized: false,
      isMinimized: false,
      ...props
    };
    setWindows([...windows, newWindow]);
    setNextZIndex(nextZIndex + 1);
  };

  const closeWindow = (id) => {
    setWindows(windows.filter(w => w.id !== id));
  };

  const minimizeWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMinimized: true } : w
    ));
  };

  const maximizeWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMaximized: !w.isMaximized } : w
    ));
  };

  const restoreWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMinimized: false, zIndex: nextZIndex } : w
    ));
    setNextZIndex(nextZIndex + 1);
  };

  const bringToFront = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, zIndex: nextZIndex } : w
    ));
    setNextZIndex(nextZIndex + 1);
  };

  const renderWindowContent = (window) => {
    switch (window.type) {
      case 'calculator':
        return <Calculator />;
      case 'texteditor':
        return <TextEditor user={user} />;
      case 'filebrowser':
        return <FileBrowser user={user} />;
      case 'webbrowser':
        return <WebBrowser />;
      case 'games':
        return <GamesLauncher onOpenGame={(game) => openWindow('game', game.name, { game })} />;
      case 'game':
        return <GameWindow game={window.game} />;
      default:
        return <div>Unknown application</div>;
    }
  };

  return (
    <div className="desktop">
      {/* Desktop Icons */}
      <div className="desktop-icons">
        <div className="desktop-icon" onDoubleClick={() => openWindow('filebrowser', 'File Browser')}>
          <div className="icon">📁</div>
          <div className="icon-label">My Files</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('calculator', 'Calculator')}>
          <div className="icon">🔢</div>
          <div className="icon-label">Calculator</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('texteditor', 'Text Editor')}>
          <div className="icon">📝</div>
          <div className="icon-label">Notepad</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('webbrowser', 'Web Browser')}>
          <div className="icon">🌐</div>
          <div className="icon-label">Browser</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('games', 'Games')}>
          <div className="icon">🎮</div>
          <div className="icon-label">Games</div>
        </div>
      </div>

      {/* Windows */}
      {windows.filter(w => !w.isMinimized).map(window => (
        <Window
          key={window.id}
          id={window.id}
          title={window.title}
          onClose={closeWindow}
          onMinimize={minimizeWindow}
          onMaximize={maximizeWindow}
          isMaximized={window.isMaximized}
          zIndex={window.zIndex}
        >
          {renderWindowContent(window)}
        </Window>
      ))}

      {/* Taskbar */}
      <div className="taskbar">
        <button className="start-btn">
          🏠 Start
        </button>
        
        <div className="taskbar-items">
          {windows.map(window => (
            <button
              key={window.id}
              className={`taskbar-item ${window.isMinimized ? 'minimized' : ''}`}
              onClick={() => window.isMinimized ? restoreWindow(window.id) : bringToFront(window.id)}
            >
              {window.title}
            </button>
          ))}
        </div>
        
        <div className="system-tray">
          <span className="time">{currentTime.toLocaleTimeString()}</span>
          <button className="logout-btn" onClick={onLogout}>Logout</button>
        </div>
      </div>
    </div>
  );
};

// Main App Component
function App() {
  const [user, setUser] = useState(null);

  const handleLogin = (userData) => {
    setUser(userData);
  };

  const handleLogout = () => {
    setUser(null);
  };

  return (
    <div className="App">
      {user ? (
        <Desktop user={user} onLogout={handleLogout} />
      ) : (
        <Login onLogin={handleLogin} />
      )}
    </div>
  );
}

export default App;